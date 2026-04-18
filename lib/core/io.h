#ifndef IO_H
#define IO_H

#include "lib/core/types.h"

unsigned char inb(unsigned short port);
unsigned short inw(unsigned short port);
void outb(unsigned short port, unsigned char val);
void outw(unsigned short port, unsigned short val);

void cpu_halt(void);
void cpu_idle(void);
void cpu_sleep_ms(uint32_t ms);

void cpu_shutdown(void);
void cpu_reset(void);

#endif
