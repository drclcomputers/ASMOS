#ifndef SERMOUSE_H
#define SERMOUSE_H

#include "lib/core.h"

/*
 * Serial mouse on COM1 (0x3F8, IRQ 4) or COM2 (0x2F8, IRQ 3).
 * Microsoft protocol (1200 baud 7N1, 3-byte packets) with the Logitech
 * 4th byte for the middle button. Detected by toggling DTR/RTS: the mouse
 * powers up and answers 'M' ("M3" Logitech, "MZ" wheel).
 * Bytes are taken in the UART interrupt (8250/16450 UARTs have no FIFO)
 * and decoded in sermouse_poll() (from the timer interrupt while the
 * desktop runs, else once a frame).
 */

bool sermouse_init(void); /* probe COM1 then COM2 */
bool sermouse_detected(void);
int sermouse_port(void);  /* 1 or 2, 0 = none */
void sermouse_poll(void); /* decode buffered bytes (mouse_apply) */
void sermouse_irq_handler(void);

#endif
