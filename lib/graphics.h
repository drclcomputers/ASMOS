#ifndef PRIMITIVE_GRAPHICS_H
#define PRIMITIVE_GRAPHICS_H

#include "config/config.h"
#include "fonts/fonts.h"
#include "lib/color.h"
#include "lib/core.h"
#include "lib/math.h"
#include "lib/memory.h"

void draw_dot(int x, int y, unsigned char color);
void draw_line(int x0, int y0, int x1, int y1, unsigned char color);
void draw_rect(int x, int y, int w, int h, unsigned char color);
void fill_rect(int x, int y, int w, int h, unsigned char color);
/* copy a block of pixels (row pitch `stride`), clipped */
void draw_pixels(int x, int y, int w, int h, const uint8_t *src, int stride);
void draw_char(int x, int y, char c, unsigned char color, int size);
void draw_string(int x, int y, const char *str, unsigned char color, int size);
void draw_text_n(int x, int y, const char *str, int n, unsigned char color,
                 int size);
int text_width(const char *str, int size);
int text_width_n(const char *str, int n, int size);
int text_fit(const char *str, int width, int size); /* chars that fit */
int font_height(int size);                          /* rows */

/* Clip rectangle for all drawing (screen coordinates). */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_get_clip(int *x, int *y, int *w, int *h);
void gfx_reset_clip(void);
void blit(void);
void clear_screen(unsigned char color);

#endif
