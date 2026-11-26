// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* A selection box: dragged out with the mouse, moved by its inside,
 * resized by its edges and corners (8 handles). What a paint program, a
 * picture or a layout editor needs; ASMDraw's selection is one.
 */

#ifndef SELBOX_H
#define SELBOX_H

#include "lib/core.h"

typedef enum {
    SB_NONE, /* outside: a new box */
    SB_INSIDE,
    SB_N,
    SB_S,
    SB_E,
    SB_W,
    SB_NW,
    SB_NE,
    SB_SW,
    SB_SE,
} sb_part_t;

typedef struct {
    bool on;          /* there is a box */
    int x, y, w, h;   /* where: (x, y) its top left, w x h (at least 1) */
    int grip;         /* how near an edge grabs it (0: 2 units) */
    int min_w, min_h; /* (0: 1) */
    /* while dragging (selbox_down to selbox_up) */
    bool dragging;
    sb_part_t part;     /* what was grabbed (SB_NONE: making) */
    bool making;        /* a new one being dragged out */
    int ax, ay;         /* where the mouse went down */
    int ox, oy, ow, oh; /* the box then */
} selbox_t;

/* what is at (x, y): SB_INSIDE, an edge or corner, or SB_NONE */
sb_part_t selbox_hit(const selbox_t *b, int x, int y);
/* the mouse went down at (x, y): what it grabbed (see above) */
sb_part_t selbox_down(selbox_t *b, int x, int y);
/* the mouse moved, held: true if the box changed. keep: a corner keeps
 * the box's shape (Shift), a new box is a square */
bool selbox_drag(selbox_t *b, int x, int y, bool keep);
/* the mouse came up: true if there is a box */
bool selbox_up(selbox_t *b);
static inline void selbox_clear(selbox_t *b) {
    b->on = b->making = b->dragging = false;
}
static inline void selbox_set(selbox_t *b, int x, int y, int w, int h) {
    b->on = true;
    b->making = b->dragging = false;
    b->x = x;
    b->y = y;
    b->w = w < 1 ? 1 : w;
    b->h = h < 1 ? 1 : h;
}
static inline bool selbox_contains(const selbox_t *b, int x, int y) {
    return b->on && x >= b->x && x < b->x + b->w && y >= b->y &&
           y < b->y + b->h;
}

int selbox_pointer(sb_part_t part);
int selbox_pointer_at(const selbox_t *b, int x, int y);

void selbox_draw(const selbox_t *b, int sx, int sy, int zoom, int cx, int cy,
                 int cw, int ch);

#endif
