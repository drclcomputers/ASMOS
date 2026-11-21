#ifndef POINTER_H
#define POINTER_H

#include "lib/core.h"

/*
 * Draws the pointer directly on screen, outside the back buffer. The timer
 * moves it as mouse input arrives; the main loop selects its shape. The last
 * frame restores the area beneath it. IRQ calls are safe; main-loop calls
 * disable interrupts while drawing.
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
