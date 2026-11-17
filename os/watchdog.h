#ifndef OS_WATCHDOG_H
#define OS_WATCHDOG_H

/*
 * Busy cursor and hung-app keys.
 *
 * The main loop calls wd_heartbeat() a few times per frame. When it has
 * not for WD_BUSY_MS (a long disk job, or an app stuck in a loop), the
 * timer interrupt takes over the bits the loop would do:
 *   - shows the hourglass over the screen and lets it follow the mouse;
 *   - reads the keyboard itself, so these still work:
 *       Win+Shift+Q  or Ctrl+Shift+Q    end the app that is stuck
 *       Ctrl+Alt+Win or Ctrl+Alt+Del    stop it and open Monitor (it is
 *                                       listed "not responding" there)
 * An app can only be stopped while it is running its own code (or kernel
 * code it called): those calls are guarded (os/appguard.h).
 *
 * When the loop is alive the same keys are handled normally (scheduler):
 * the first ends the focused app, the second opens Monitor.
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
