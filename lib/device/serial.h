#ifndef SERIAL_H
#define SERIAL_H

#include "lib/core.h"

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_write_hex(uint32_t v);

#endif
