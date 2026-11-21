#ifndef SERMOUSE_H
#define SERMOUSE_H

#include "lib/core.h"

/*
 * Microsoft-protocol serial mouse on COM1/COM2 (1200 baud, 7N1), with the
 * Logitech fourth-byte middle button. Toggle DTR/RTS to detect it; read
 * bytes in the UART IRQ and decode in sermouse_poll().
 */

bool sermouse_init(void); /* probe COM1 then COM2 */
bool sermouse_detected(void);
int sermouse_port(void);  /* 1 or 2, 0 = none */
void sermouse_poll(void); /* decode buffered bytes (mouse_apply) */
void sermouse_irq_handler(void);

#endif
