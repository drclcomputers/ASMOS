#ifndef UTILS_H
#define UTILS_H

#include "lib/math.h"
#include "lib/mem.h"
#include "lib/primitive_graphics.h"
#include "lib/string.h"
#include "lib/types.h"

// Manage Screen
void clear_screen_fast(unsigned char color);
void delay(int count);

// IO
void outb(unsigned short port, unsigned char val);

#endif
