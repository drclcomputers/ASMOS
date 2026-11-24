// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "lib/io.h"
#include "config/config.h"
#include "drivers/gpu.h"
#include "lib/graphics.h"

unsigned char inb(unsigned short port) {
    unsigned char ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

unsigned short inw(unsigned short port) {
    unsigned short ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t inl(unsigned short port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outb(unsigned short port, unsigned char val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

void outw(unsigned short port, unsigned short val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

void outl(unsigned short port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

void cpu_halt(void) { asm volatile("hlt"); }

void cpu_idle(void) { asm volatile("sti; hlt"); }

void wd_stop(void);
#include "lib/apm.h"

void cpu_shutdown(void) {
    wd_stop();
    apm_power_off();      /* the BIOS switches it off (lib/apm.h) */
    outw(0x604, 0x2000);  /* QEMU */
    outw(0xB004, 0x2000); /* Bochs / old QEMU */
    /* Still running: real hardware (no ACPI/APM driver yet). Say so, the
     * way DOS-era Windows did. */
    for (volatile uint32_t i = 0; i < 0x100000; i++)
        ;
    clear_screen(BLACK);
    const char *a = "It's now safe to turn off";
    const char *b = "your computer.";
    draw_string((SCREEN_WIDTH - text_width(a, 1)) / 2, SCREEN_HEIGHT / 2 - 12,
                a, LIGHT_RED, 1);
    draw_string((SCREEN_WIDTH - text_width(b, 1)) / 2, SCREEN_HEIGHT / 2 + 2, b,
                LIGHT_RED, 1);
    gpu_invalidate();
    blit();
    __asm__ volatile("cli");
    for (;;)
        __asm__ volatile("hlt");
}

void cpu_reset(void) {
    uint8_t v;
    do {
        v = inb(0x64);
    } while (v & 0x02);
    outb(0x64, 0xFE);
    struct __attribute__((packed)) {
        uint16_t limit;
        uint32_t base;
    } idt = {0, 0};
    __asm__ volatile("lidt %0; int $0x00" : : "m"(idt));
    for (;;)
        __asm__ volatile("hlt");
}
