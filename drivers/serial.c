// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/serial.h"
#include "lib/io.h"

#define COM1_PORT 0x3F8

uint16_t com_base(int n) {
    return n >= 0 && n < 4 ? bios_data_word((uint32_t)n * 2) : 0;
}

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x80);
    outb(COM1_PORT + 0, 0x01);
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03);
    outb(COM1_PORT + 2, 0xC7);
    outb(COM1_PORT + 4, 0x0B);
}

static int serial_is_transmit_empty(void) { return inb(COM1_PORT + 5) & 0x20; }

void serial_putc(char c) {
    while (!serial_is_transmit_empty()) {
        asm volatile("nop");
    }
    outb(COM1_PORT + 0, (unsigned char)c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
            serial_putc('\n');
            s++;
            continue;
        }
        serial_putc(*s++);
    }
}

void serial_write_hex(uint32_t v) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; --i) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        serial_putc(hex[nib]);
    }
}
