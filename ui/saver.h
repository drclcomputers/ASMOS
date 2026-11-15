#ifndef SAVER_H
#define SAVER_H

#include "lib/core.h"

/* Screen saver. After g_cfg.saver_minutes without input the desktop is
 * replaced by an effect; any key or mouse movement brings it back (that
 * input is swallowed, so a click can't land on a window). */

enum { SAVER_STARS, SAVER_LINES, SAVER_RAIN, SAVER_RANDOM, SAVER_COUNT };

extern const char *const SAVER_NAMES[SAVER_COUNT];

/* main loop, right after input is read: true while the saver is up (it
 * has drawn the frame; skip the desktop) */
bool saver_update(void);
void saver_draw(void);  /* whole screen */
void saver_start(void); /* now (Settings "Test") */
bool saver_active(void);

/* one frame of an effect inside a rectangle (also the Settings preview) */
void saver_paint(int x, int y, int w, int h, int effect);

#endif
