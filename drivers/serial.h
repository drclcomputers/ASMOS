#ifndef SERIAL_H
#define SERIAL_H

#include "lib/core.h"

/* COM1 debug output. Not called anywhere; kept as a debugging aid
 * (call serial_init() early in kmain to use it). */

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_write_hex(uint32_t v);

#endif
