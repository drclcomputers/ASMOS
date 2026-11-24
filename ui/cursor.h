// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef CURSOR_H
#define CURSOR_H

#include "lib/core.h"
#include "lib/graphics.h"

extern int CHOSEN_CURSOR;

void draw_cursor(int x, int y);
/* the pointer shapes (9x8, 0xFF = see-through, hot spot 0,0) */
const uint8_t *cursor_sprite(int shape);
/* main loop, once a frame: the pointer on (shape from CHOSEN_CURSOR, or
 * the hourglass while busy) or off (screen saver) */
void cursor_frame(bool on);

#endif
