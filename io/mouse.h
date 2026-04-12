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

void mouse_init(void);
void mouse_update(void);
void mouse_process_byte(uint8_t raw);

#endif
