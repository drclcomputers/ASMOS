#ifndef IDT_H
#define IDT_H

#include "lib/core.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define IRQ0_VECTOR 32

#define PIT_CHANNEL0 0x40
#define PIT_CMD 0x43

#define PIT_DIVISOR 1193

extern volatile uint32_t pit_seconds;

void idt_init(void);
void idt_emulate_fpu(void); /* #NM -> lib/fpu_emu.c */
/* Point IRQ `irq` (0-15) at `stub` and unmask it (and the cascade). The
 * handler must send EOI to the slave PIC (0xA0) too when irq >= 8. */
void idt_set_irq(int irq, void (*stub)(void));
void idt_mask_irq(int irq);
/* Already given to a driver? (ISA IRQs can't be shared by our stubs) */
bool idt_irq_in_use(int irq);

#endif
