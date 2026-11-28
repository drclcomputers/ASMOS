// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/mousemove.h"
#include "config/runtime_config.h"

/* the pointer's travel for a count of the mouse, in eighths: 5 is one to
 * one; slower ones keep the rest of a move for the next */
static const uint8_t SPEED[MOUSE_SPEEDS] = {3, 4, 5, 6, 8, 10, 12, 14, 16};
/* a double-click's two clicks at most this far apart, ms */
static const uint16_t DBL[MOUSE_SPEEDS] = {900, 780, 660, 550, 450,
                                           380, 320, 260, 200};

static int level(uint8_t v) { return v >= 1 && v <= MOUSE_SPEEDS ? v : 5; }

void mouse_shape(int *dx, int *dy, bool *left, bool *right) {
    static int rx, ry; /* (eighths left over) */
    int f = SPEED[level(g_cfg.mouse.speed) - 1];
    if (f != 8) {
        int x = *dx * f + rx, y = *dy * f + ry;
        *dx = x / 8;
        *dy = y / 8;
        rx = x - *dx * 8;
        ry = y - *dy * 8;
    }
    if (g_cfg.mouse.swap) {
        bool l = *left;
        *left = *right;
        *right = l;
    }
}

uint32_t mouse_dblclick_ms_at(int speed) {
    return DBL[level((uint8_t)(speed ? speed : g_cfg.mouse.dbl)) - 1];
}

uint32_t mouse_dblclick_ms(void) { return mouse_dblclick_ms_at(0); }
