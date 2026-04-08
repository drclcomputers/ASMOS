#include "io/ps2.h"
#include "fs/fat16.h"
#include "ui/menubar.h"
#include "os/os.h"
#include "os/error.h"
#include "shell/cli.h"
#include "config/config.h"
#include "lib/alloc.h"
#include "lib/primitive_graphics.h"
#include "interrupts/idt.h"
#include "lib/time.h"
#include "ui/desktop.h"
#include "ui/desktop_fs.h"

extern app_descriptor clock_app;
extern app_descriptor finder_app;
extern app_descriptor asmterm_app;
extern app_descriptor monitor_app;

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

    boot_banner();
    sleep_s(1);

    boot_check_heap();
    fat16_mount();
    boot_check_ata();
    boot_check_fat();

    sleep_s(2);

    {
        volatile uint32_t d = 0x1000000;
        while (d--) __asm__ volatile ("nop");
    }

    if (!START_IN_GUI) cli_run();

    desktop_fs_init();

    desktop_init();
    menubar_init();

    error_set_gui_mode(true);

    os_install_app(&clock_app);
    os_install_app(&finder_app);
    os_install_app(&asmterm_app);
    os_install_app(&monitor_app);

    os_run();
}
