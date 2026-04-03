#include "io/ps2.h"
#include "fs/fat16.h"
#include "ui/menubar.h"
#include "os/os.h"
#include "shell/cli.h"
#include "config/config.h"
#include "lib/alloc.h"
#include "interrupts/idt.h"

extern app_descriptor finder_app;
extern app_descriptor terminal_app;

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

void kmain(void) {
    alloc_init();
    detect_heap_end();

    idt_init();

    ps2_init();
    fat16_mount();

    menubar_init();

    os_register_app(&finder_app);
    os_register_app(&terminal_app);

    if (!START_IN_GUI) cli_run();

    os_run();
}
