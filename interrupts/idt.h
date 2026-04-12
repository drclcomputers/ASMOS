#ifndef IDT_H
#define IDT_H

#include "lib/core.h"

#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1

#define IRQ0_VECTOR 32

#define PIT_CHANNEL0  0x40
#define PIT_CMD       0x43

#define PIT_DIVISOR   11931

extern volatile uint32_t pit_ticks;
extern volatile uint32_t pit_seconds;

void idt_init(void);

#endif
