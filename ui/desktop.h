#ifndef DESKTOP_H
#define DESKTOP_H

#include "lib/core.h"

void draw_wallpaper_pattern(void);
void desktop_init(void);
void desktop_on_frame(void); /* input and logic; draws nothing */
void desktop_draw(void);     /* wallpaper + icons */
uint32_t desktop_sig(void);

void power_anim_run(bool is_restart);

#endif
