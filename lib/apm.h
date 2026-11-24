// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef APM_H
#define APM_H

#include "lib/core.h"

/*
 * BIOS APM power control. Stage 2 connects the 32-bit interface in real mode;
 * apm_init() installs its GDT entries. Power-off requires APM 1.0+ and a
 * board/power supply that supports it.
 */

void apm_init(void); /* kmain, after the IDT */
bool apm_present(void);
uint16_t apm_version(void); /* BCD: 0x0102 = 1.2; 0 = none */
/* Turn the machine off. Returns only if it did not happen. */
void apm_power_off(void);

#endif
