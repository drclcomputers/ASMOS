#ifndef MOUSE_H
#define MOUSE_H

#include "lib/types.h"
#include "lib/utils.h"

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

#endif
