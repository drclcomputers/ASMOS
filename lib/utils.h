#ifndef UTILS_H
#define UTILS_H

#include "lib/io.h"
#include "lib/math.h"
#include "lib/mem.h"
#include "lib/primitive_graphics.h"
#include "lib/string.h"
#include "lib/types.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200

// Manage Screen
#define BACKBUF ((uint8_t*)0x100000)
void blit(void);
void clear_screen(unsigned char color);
void delay(int count);

#endif
