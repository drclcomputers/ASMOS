// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef OS_WATCHDOG_H
#define OS_WATCHDOG_H

/*
 * If wd_heartbeat() misses WD_BUSY_MS, the timer shows the busy cursor and
 * handles Win+Shift+Q/Ctrl+Shift+Q (end app) and Ctrl+Alt+Win/Ctrl+Alt+Del
 * (open Monitor). Stopping requires guarded code; the scheduler handles
 * these keys when responsive.
 */

#include "lib/core.h"

#define WD_BUSY_MS 600

void wd_heartbeat(void); /* main loop: "still here" */
void wd_stop(void);      /* leaving the GUI: the IRQ side goes quiet */
void wd_tick(void);      /* timer IRQ, every tick */

/* Show the hourglass now (long job ahead) / until the next frame. */
void wd_busy(void);
/* the hourglass is on the screen (the arrow pointer then is not drawn) */
bool wd_busy_shown(void);

/* Main loop, once a frame: what the IRQ asked for. */
bool wd_take_monitor_request(void);

#endif
