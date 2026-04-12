#include "lib/graphics/primitive_graphics.h"
#include "lib/math.h"
#include "fonts/fonts.h"
#include "lib/core.h"
#include "lib/memory.h"

void draw_dot(int x, int y, unsigned char color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    ((unsigned char *)BACKBUF)[y * SCREEN_WIDTH + x] = color;
}

void draw_line(int x0, int y0, int x1, int y1, unsigned char color) {
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        draw_dot(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void draw_rect(int x, int y, int w, int h, unsigned char color) {
    draw_line(x,         y,         x + w - 1, y,         color);
    draw_line(x,         y,         x,         y + h - 1, color);
    draw_line(x,         y + h - 1, x + w - 1, y + h - 1, color);
    draw_line(x + w - 1, y,         x + w - 1, y + h - 1, color);
}

void fill_rect(int x, int y, int w, int h, unsigned char color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > SCREEN_WIDTH  ? SCREEN_WIDTH  : x + w;
    int y1 = y + h > SCREEN_HEIGHT ? SCREEN_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    unsigned char *buf = (unsigned char *)BACKBUF;
    for (int row = y0; row < y1; row++)
        memset(buf + row * SCREEN_WIDTH + x0, color, x1 - x0);
}

void draw_char(int x, int y, char c, unsigned char color, int size) {
    if (y < 0 || y >= SCREEN_HEIGHT) return;
    int offset, maxrow, maxcol;
    unsigned char startbit;
    if (size == 1) {
        offset = (c - 32) * 8; maxrow = 8; maxcol = 8; startbit = 0x80;
    } else {
        offset = (c - 32) * 6; maxrow = 6; maxcol = 4; startbit = 0x08;
    }
    for (int row = 0; row < maxrow; row++) {
        if (y + row < 0 || y + row >= SCREEN_HEIGHT) continue;
        unsigned char row_data = (size == 1) ? font1_data[offset + row]
                                             : font2_data[offset + row];
        for (int col = 0; col < maxcol; col++) {
            if (row_data & (startbit >> col))
                draw_dot(x + col, y + row, color);
        }
    }
}

void draw_string(int x, int y, char *str, unsigned char color, int size) {
    int s = (size == 1) ? 8 : 5;
    for (int i = 0; str[i]; i++) {
        int cx = x + i * s;
        if (cx >= SCREEN_WIDTH) break;
        if (cx + s > 0)
            draw_char(cx, y, str[i], color, size);
    }
}

void blit(void) {
    uint32_t *src = (uint32_t *)BACKBUF;
    uint32_t *dst = (uint32_t *)0xA0000;
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT / 4; i++)
        dst[i] = src[i];
}

void clear_screen(unsigned char color) {
    uint32_t *buf = (uint32_t *)BACKBUF;
    uint32_t  val = color | (color << 8) | (color << 16) | (color << 24);
    for (uint32_t i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT / 4; i++)
        buf[i] = val;
}

void delay(int count) {
    volatile int i;
    for (i = 0; i < count; i++)
        __asm__("nop");
}
