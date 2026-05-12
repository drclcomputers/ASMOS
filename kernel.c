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
int g_screen_width = 640;
int g_screen_height = 400;
int g_video_mode = 1;
int g_backbuf_size = 640 * 400;

extern void wm_init(void);
extern void scheduler_init(void);
extern void desktop_on_frame(void);

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} e820_entry_t;

static void detect_heap_range(void) {
    uint16_t count = *(volatile uint16_t *)0x0500;

    uint32_t kernel_end = (uint32_t)&_heap_start;
    if (kernel_end < HEAP_MIN_START)
        kernel_end = HEAP_MIN_START;

    kernel_end = (kernel_end + 15U) & ~15U;

    uint32_t heap_top = 0;

    if (count > 0 && count <= 50) {
        uint8_t *ptr = (uint8_t *)0x0504;

        for (uint16_t i = 0; i < count; i++, ptr += 20) {
            e820_entry_t *e = (e820_entry_t *)ptr;

            if (e->type != 1)
                continue;

            uint64_t base = e->base;
            uint64_t end64 = base + e->length;

            if (base > (uint64_t)kernel_end)
                continue;
            if (end64 <= (uint64_t)kernel_end)
                continue;

            uint32_t end32 =
                (end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)end64;

            if (end32 > heap_top)
                heap_top = end32;
        }
    }

    if (heap_top == 0) {
        heap_top = HEAP_END_MAX;
    }

    if (heap_top > HEAP_END_MAX)
        heap_top = HEAP_END_MAX;

    if (heap_top <= kernel_end + 0x10000U) {
        for (;;)
            __asm__ volatile("hlt");
    }

    alloc_set_range(kernel_end, heap_top);
}

static void boot_banner(void) {
    clear_screen(BLACK);
    draw_string(4, 2, "ASMOS Boot", WHITE, 2);
    blit();
}

static void resolution_set(void) {
    g_video_mode = *(volatile uint8_t *)0x0602;
    g_screen_width = *(volatile uint16_t *)0x0604;
    g_screen_height = *(volatile uint16_t *)0x0606;
    
    if (g_screen_width == 0 || g_screen_height == 0) {
        if (RESMODE) {
            g_video_mode = 1;
            g_screen_width = 640;
            g_screen_height = 400;
        } else {
            g_video_mode = 0;
            g_screen_width = 320;
            g_screen_height = 200;
        }
    }
    
    g_backbuf_size = g_screen_width * g_screen_height;
}

void kmain(void) {
    resolution_set();

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
