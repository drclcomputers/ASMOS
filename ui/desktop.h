// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef DESKTOP_H
#define DESKTOP_H

#include "lib/core.h"

void draw_wallpaper_pattern(void);
void desktop_init(void);
/* runs a .LNK shortcut's target (see desktop.c); false if it has none */
bool desktop_open_shortcut(const char *path);
void desktop_on_frame(void); /* input and logic; draws nothing */
void desktop_draw(void);     /* wallpaper + icons */
uint32_t desktop_sig(void);

void power_anim_run(bool is_restart);

/* /APPS/CDROM: shortcuts to the apps on the CD in the drive (none: gone) */
void desktop_cd_links(void);

#endif
