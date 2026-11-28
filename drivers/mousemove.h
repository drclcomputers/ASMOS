// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef MOUSEMOVE_H
#define MOUSEMOVE_H

/* What the mouse settings (g_cfg.mouse) do to what a mouse reports: the
 * pointer's speed, the buttons swapped, the double-click time. The PS/2
 * and the serial driver send their moves through mouse_shape. */

#include "lib/core.h"

#define MOUSE_SPEEDS 9

void mouse_shape(int *dx, int *dy, bool *left, bool *right);

uint32_t mouse_dblclick_ms_at(int speed);
uint32_t mouse_dblclick_ms(void);

#endif
