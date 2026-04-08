#ifndef DESKTOP_H
#define DESKTOP_H

#include "lib/types.h"

void draw_wallpaper_pattern(void);
void desktop_init(void);
void desktop_on_frame(void);

bool desktop_accept_drop(const char *src_path, const char *src_name);

#endif
