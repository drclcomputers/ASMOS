#ifndef APM_H
#define APM_H

#include "lib/core.h"

/*
 * APM (Advanced Power Management): lets the BIOS switch the PC off.
 * Stage 2 connects the BIOS's 32-bit interface while still in real mode
 * (boot/stage2.asm) and leaves its segments at 0x0610 (bootdefs.inc);
 * apm_init() makes GDT entries 0x18/0x20/0x28 from them.
 *
 * Needs a BIOS with APM 1.0+ (most from 1993 on) and a power supply the
 * board can switch (ATX); a 386 board with an AT supply has neither, and
 * ASMOS then says it is safe to turn off.
 */

void apm_init(void); /* kmain, after the IDT */
bool apm_present(void);
uint16_t apm_version(void); /* BCD: 0x0102 = 1.2; 0 = none */
/* Turn the machine off. Returns only if it did not happen. */
void apm_power_off(void);

#endif
