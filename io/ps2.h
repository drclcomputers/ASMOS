#ifndef PS2_H
#define PS2_H

#include "io/mouse.h"
#include "io/keyboard.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

void ps2_init(void);
void ps2_update(void);

#endif
