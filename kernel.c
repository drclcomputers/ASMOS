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

static void detect_heap_range(void) {
    uint16_t count = *(volatile uint16_t *)0x500;
    uint8_t *ptr = (uint8_t *)0x504;
    uint32_t best = HEAP_END_MAX;

    uint32_t kernel_end = (uint32_t)&_heap_start;
    if (kernel_end < HEAP_MIN_START)
        kernel_end = HEAP_MIN_START;

    for (uint16_t i = 0; i < count; i++, ptr += 20) {
        e820_entry_t *e = (e820_entry_t *)ptr;
        if (e->type != 1)
            continue;
        uint64_t base = e->base;
        uint64_t end64 = base + e->length;
        if (base <= kernel_end && end64 > kernel_end) {
            uint32_t end32 =
                (end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)end64;
            if (end32 > best)
                best = end32;
        }
    }

    // Never exceed the bottom of the kernel stack
    if (best > HEAP_END_MAX)
        best = HEAP_END_MAX;

    if (best <= kernel_end) {
        while (1) {
        } // no usable memory – halt
    }

    alloc_set_range(kernel_end, best);
}

uint32_t g_vesa_fb = 0;

static void boot_banner(void) {
    clear_screen(BLACK);
    draw_string(4, 2, "ASMOS Boot", WHITE, 2);
    blit();
}

void kmain(void) {
    detect_heap_range();
    idt_init();
    ps2_init();

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
