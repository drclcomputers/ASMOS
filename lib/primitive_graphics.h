#ifndef PRIMITIVE_GRAPHICS_H
#define PRIMITIVE_GRAPHICS_H

#include "lib/types.h"
#include "lib/mem.h"
#include "lib/math.h"
#include "config/config.h"

void draw_dot(int x, int y, unsigned char color);
void draw_line(int x0, int y0, int x1, int y1, unsigned char color);
void draw_rect(int x, int y, int w, int h, unsigned char color);
void fill_rect(int x, int y, int w, int h, unsigned char color);
void draw_char(int x, int y, char c, unsigned char color, int size);
void draw_string(int x, int y, char *str, unsigned char color, int size);
void blit(void);
void clear_screen(unsigned char color);
void delay(int count);

#endif
