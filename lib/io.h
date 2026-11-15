#ifndef IO_H
#define IO_H

#include "lib/types.h"

unsigned char inb(unsigned short port);
unsigned short inw(unsigned short port);
uint32_t inl(unsigned short port);
void outb(unsigned short port, unsigned char val);
void outw(unsigned short port, unsigned short val);
void outl(unsigned short port, uint32_t val);

/* Word from the BIOS data area (0x400 + off): COM bases at 0x400, LPT
 * bases at 0x408. Through asm so GCC doesn't flag the low address. */
static inline uint16_t bios_data_word(uint32_t off) {
    uint16_t v;
    __asm__ volatile("movw (%1), %0" : "=r"(v) : "r"(0x400 + off) : "memory");
    return v;
}

void cpu_halt(void);
void cpu_idle(void);

void cpu_shutdown(void);
void cpu_reset(void);

#endif
