// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef PS2_H
#define PS2_H

#include "drivers/keyboard.h"
#include "drivers/mouse.h"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64

void ps2_init(void);
void ps2_update(void); /* main loop, start of frame */

/* Desktop running: the timer interrupt reads the controller (ps2.c). */
void ps2_irq_mode(bool on);
bool ps2_irq_active(void);
void ps2_irq_poll(void); /* timer interrupt, every tick */
int ps2_key_byte(void);  /* next buffered key byte, -1 = none */

#endif
