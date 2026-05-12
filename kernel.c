#include "fs/fs.h"
#include "io/ps2.h"

#include "drivers/gpu.h"
#include "drivers/opl2.h"
#include "drivers/sb16.h"

#include "os/app_registry.h"
#include "os/error.h"
#include "os/os.h"
#include "os/scheduler.h"
#include "shell/cli.h"

#include "config/config.h"
#include "config/runtime_config.h"

#include "interrupts/idt.h"
#include "lib/device.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/time.h"

#include "ui/ui.h"

extern uint8_t _heap_start;

extern void wm_init(void);
extern void scheduler_init(void);
extern void desktop_on_frame(void);

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} e820_entry_t;

/* ── detect_heap_range ────────────────────────────────────────────────────────
 *
 * Reads the E820 map stored by stage2 at:
 *   0x0500 (word)  – number of entries (0 if E820 not supported)
 *   0x0504+        – entries, 20 bytes each
 *
 * Goal: find the largest contiguous usable (type=1) region that contains
 * the kernel end address, then use from kernel_end to min(region_top,
 * HEAP_END_MAX).
 *
 * Fallback for machines without INT 15h/E820 (some old 486 BIOSes):
 *   Use INT 15h/E801 result stored at 0x0502 (not populated by our stage2 yet
 *   — see note), or simply trust that on a 4 MB machine there is contiguous
 *   RAM from 0 to 0x400000.  We conservatively cap at HEAP_END_MAX (0x3F0000)
 *   which is always safe for a 4 MB machine.
 *
 * On the Samsung SPC7700P-LW with 4 MB:
 *   E820 will typically report:
 *     0x00000000–0x0009FFFF  type 1 (conventional, 640 KB)
 *     0x000F0000–0x000FFFFF  type 2 (ROM)
 *     0x00100000–0x003FFFFF  type 1 (extended, 3 MB)
 *   The second type-1 entry contains our kernel_end and its top is 0x400000.
 *   We cap at HEAP_END_MAX (0x3F0000) to leave 64 KB before the stack.
 */
static void detect_heap_range(void) {
    uint16_t count = *(volatile uint16_t *)0x0500;

    uint32_t kernel_end = (uint32_t)&_heap_start;
    if (kernel_end < HEAP_MIN_START)
        kernel_end = HEAP_MIN_START;

    /* Align kernel_end to a 16-byte boundary */
    kernel_end = (kernel_end + 15U) & ~15U;

    uint32_t heap_top = 0;

    if (count > 0 && count <= 50) {
        /* E820 is available – walk the map */
        uint8_t *ptr = (uint8_t *)0x0504;

        for (uint16_t i = 0; i < count; i++, ptr += 20) {
            e820_entry_t *e = (e820_entry_t *)ptr;

            if (e->type != 1)
                continue; /* not usable RAM */

            uint64_t base = e->base;
            uint64_t end64 = base + e->length;

            /* We want the region that contains kernel_end */
            if (base > (uint64_t)kernel_end)
                continue;
            if (end64 <= (uint64_t)kernel_end)
                continue;

            /* Clamp to 32-bit address space */
            uint32_t end32 =
                (end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)end64;

            if (end32 > heap_top)
                heap_top = end32;
        }
    }

    if (heap_top == 0) {
        /*
         * E820 not available or no matching region found.
         *
         * Safe fallback: on a 4 MB machine extended memory runs from
         * 0x100000 to 0x3FFFFF.  Use HEAP_END_MAX which is defined as
         * 0x3F0000 – this always leaves room for the kernel stack at
         * 0x3F0000–0x3F8000.
         *
         * For machines with more RAM this is conservative but harmless;
         * the OS will simply use less heap.
         */
        heap_top = HEAP_END_MAX;
    }

    /* Never exceed the bottom of the kernel stack */
    if (heap_top > HEAP_END_MAX)
        heap_top = HEAP_END_MAX;

    /* Sanity: must have at least 64 KB of heap */
    if (heap_top <= kernel_end + 0x10000U) {
        /* No usable memory – halt with a visible indication */
        for (;;)
            __asm__ volatile("hlt");
    }

    alloc_set_range(kernel_end, heap_top);
}

uint32_t g_vesa_fb = 0;

/* ── boot_banner ──────────────────────────────────────────────────────────────
 *
 * Clears the back-buffer explicitly before the first blit.
 * Without this, on a real machine the back-buffer region (0x100000–0x14AFFF)
 * contains whatever was left in RAM by the BIOS or previous boot, which
 * produces garbage pixels on screen until a full frame is drawn.
 */
static void boot_banner(void) {
    /* Explicit clear – memset the back-buffer before the first GPU blit */
    memset((void *)BACKBUF, BLACK, BACKBUF_SIZE);

    clear_screen(BLACK);
    draw_string(4, 2, "ASMOS Boot", WHITE, 2);
    blit();
}

void kmain(void) {
    detect_heap_range();
    idt_init();
    ps2_init();

    /*
     * gpu_init() sets the VESA/VGA mode and programs the DAC.
     * On real hardware this may take a moment for the S3 BIOS to complete.
     * The sleep below gives the monitor time to re-sync after the mode change.
     */
    gpu_init();

    sleep_s(1);
    boot_banner();
    sleep_s(1);

    boot_check_heap();
    fs_mount();
    boot_check_ata();
    boot_check_fat();

    cfg_init_defaults();
    if (!cfg_load()) {
        cfg_save();
    }

    if (g_cfg.sound_enabled) {
        speaker_init();
        sb16_init();
        opl2_init();
        sb16_unmute_fm();
    }

    boot_check_sound();

    if (g_cfg.play_bootchime && g_cfg.sound_enabled) {
        speaker_beep(523, 120);
        speaker_beep(659, 120);
        speaker_beep(784, 180);
    }

    sleep_s(2);

    /*
     * Original busy-wait: kept as-is.
     * On a real 486 @ 33-66 MHz this is a brief pause.
     * On a modern machine (if ever tested) it's near-instant.
     */
    {
        volatile uint32_t d = 0x1000000;
        while (d--)
            __asm__ volatile("nop");
    }

    if (!g_cfg.start_in_gui)
        cli_run();

    desktop_fs_init();

    wm_init();
    desktop_init();
    menubar_init();

    error_set_gui_mode(true);

    for (int i = 0; i < app_registry_count; i++)
        os_install_app(app_registry[i].desc);

    scheduler_init();

    scheduler_kernel_task();
}
