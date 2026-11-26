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

/* shapes past the 4 of CHOSEN_CURSOR (os/appapi.h APP_POINTER_*); these
 * and the I-beam are bigger, with the hot spot in the middle */
#define CURSOR_TEXT 3      /* the I-beam, 5x11 */
#define CURSOR_MOVE 4      /* four arrows, 13x13 */
#define CURSOR_SIZE_NWSE 5 /* resize: top left - bottom right */
#define CURSOR_SIZE_NESW 6 /* resize: top right - bottom left */
#define CURSOR_SIZE_WE 7   /* resize: left - right */
#define CURSOR_SIZE_NS 8   /* resize: up - down */
#define CURSOR_LAST 8
/* this frame only, over CHOSEN_CURSOR: an app's (api->pointer), an edit
 * field's I-beam. Not while a menu is open. */
void cursor_request(int shape);
int cursor_shown(void);     /* the shape last put up (tests) */
void cursor_put(int shape); /* any shape on the screen now */
void cursor_restore(void);  /* after the hourglass */

#endif
