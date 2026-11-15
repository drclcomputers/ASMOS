#ifndef MOUSE_H
#define MOUSE_H

#include "lib/core.h"

typedef struct {
    int x, y;
    int dx, dy;
    bool left;
    bool right;
    bool middle;
    bool left_clicked;
    bool right_clicked;
} mousevar;

extern mousevar mouse;

typedef enum { MOUSE_NONE = 0, MOUSE_PS2 = 1, MOUSE_SERIAL = 2 } mouse_kind_t;

/* Probe PS/2 (aux port answers) and a serial mouse on COM1/COM2; both may
 * be active. */
void mouse_init(void);
mouse_kind_t mouse_kinds(void);       /* bit mask of MOUSE_PS2 | MOUSE_SERIAL */
void mouse_update(void);              /* start of frame: clear deltas/clicks */
void mouse_process_byte(uint8_t raw); /* PS/2 aux byte */
/* Move by (dx, dy) screen pixels and set the buttons; used by every
 * mouse driver. Clicks within one frame are kept. */
void mouse_apply(int dx, int dy, bool left, bool right, bool middle);

#endif
