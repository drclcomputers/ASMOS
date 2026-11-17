#ifndef CURSOR_H
#define CURSOR_H

#include "lib/core.h"
#include "lib/graphics.h"

extern int CHOSEN_CURSOR;

void draw_cursor(int x, int y);
/* pointer on a back buffer that is kept between frames */
void cursor_show(int x, int y);   /* remember what it covers, draw it;
                                   * nothing while the hourglass is up */
void cursor_erase_screen(void);   /* the hourglass came: off the screen */
bool cursor_hide(int *y, int *h); /* put that back; rows it touched */
void cursor_forget(void);         /* everything was redrawn */
#define CURSOR_H_PX 8

#endif
