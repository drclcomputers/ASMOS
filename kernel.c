#include "io/ps2.h"
#include "fs/fat16.h"

#include "shell/cli.h"
#include "os/os.h"
#include "os/error.h"
#include "os/app_registry.h"

#include "config/config.h"
#include "config/runtime_config.h"

#include "lib/memory.h"
#include "lib/graphics.h"
#include "lib/time.h"
#include "lib/device.h"
#include "interrupts/idt.h"

#include "ui/desktop.h"
#include "ui/desktop_fs.h"
#include "ui/menubar.h"

extern app_descriptor asmdraw_app;
extern app_descriptor calculator_app;
extern app_descriptor clock_app;
extern app_descriptor filef_app;
extern app_descriptor asmterm_app;
extern app_descriptor monitor_app;
extern app_descriptor settings_app;
extern app_descriptor teditor_app;

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} e820_entry_t;

static void detect_heap_end(void) {
    uint16_t count = *(volatile uint16_t *)0x500;
    uint8_t *ptr   = (uint8_t *)0x504;
    uint32_t best  = HEAP_END;

    for (uint16_t i = 0; i < count; i++, ptr += 20) {
        e820_entry_t *e = (e820_entry_t *)ptr;
        if (e->type != 1) continue;
        if (e->base <= HEAP_START && e->base + e->length > HEAP_START) {
            uint64_t end64 = e->base + e->length;
            best = (end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)end64;
        }
    }
    alloc_set_end(best);
}

static void boot_banner(void) {
    clear_screen(BLACK);

    draw_string(4, 2, "ASMOS Boot", WHITE, 2);
    blit();
}

void kmain(void) {
    alloc_init();
    detect_heap_end();
    idt_init();
    ps2_init();

    sleep_s(1);
    boot_banner();
    sleep_s(1);

    boot_check_heap();
    fat16_mount();
    boot_check_ata();
    boot_check_fat();

    cfg_init_defaults();
    if (!cfg_load()) {
        cfg_save();
    }

    if (g_cfg.play_bootchime && g_cfg.sound_enabled) {
        speaker_beep(523, 120);
        speaker_beep(659, 120);
        speaker_beep(784, 180);
    }

    sleep_s(2);

    {
        volatile uint32_t d = 0x1000000;
        while (d--) __asm__ volatile ("nop");
    }

    if (!g_cfg.start_in_gui) cli_run();

    desktop_fs_init();

    desktop_init();
    menubar_init();
    if (g_cfg.sound_enabled) speaker_init();
    error_set_gui_mode(true);

    for (int i = 0; i < app_registry_count; i++)
    	os_install_app(app_registry[i].desc);

    os_run();
}
