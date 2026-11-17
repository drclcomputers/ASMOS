#ifndef POINTER_H
#define POINTER_H

#include "lib/core.h"

/*
 * The mouse pointer, drawn straight onto the screen (like a hardware
 * cursor): it is never in the back buffer, so moving it needs no frame.
 * The timer interrupt moves it as the mouse bytes come in (drivers/ps2.c);
 * the main loop only picks the shape. What it covers comes back from the
 * copy of the last frame the blit keeps (drivers/gpu.c).
 *
 * Everything here may be called from the timer interrupt; from the main
 * loop the calls keep interrupts off while they touch the screen.
 */

/* w*h pixels, 0xFF = see-through; (hx, hy) = the hot spot */
void pointer_shape(const uint8_t *pix, int w, int h, int hx, int hy);
void pointer_move(int x, int y); /* the hot spot goes here */
void pointer_show(bool on);      /* off: what it covered goes back */
bool pointer_visible(void);
void pointer_forget(void); /* the screen was changed under it (mode
                            * set): don't restore, just stop */
void pointer_after_blit(int y0, int y1); /* gpu.c: blit went over it */

#endif
