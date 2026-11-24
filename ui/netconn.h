// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef NETCONN_H
#define NETCONN_H

#include "ui/controls.h"

void netconn_dialog(window *owner, void (*done)(void));
void netconn_disconnect(window *owner, void (*done)(void));

#endif
