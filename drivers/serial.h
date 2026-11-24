// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef SERIAL_H
#define SERIAL_H

#include "lib/core.h"

/* COM1 debug output. Not called anywhere; kept as a debugging aid
 * (call serial_init() early in kmain to use it). */

/* COM1-4 (n = 0..3): the base port the BIOS found, 0 = absent */
uint16_t com_base(int n);

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_write_hex(uint32_t v);

#endif
