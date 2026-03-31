#include "lib/primitive_graphics.h"
#include "lib/math.h"
#include "fonts/fonts.h"
#include "lib/types.h"
#include "lib/mem.h"

void draw_dot(int x, int y, unsigned char color) {
    unsigned char* vga = BACKBUF;
    vga[y * 320 + x] = color;
}

void draw_line(int x0, int y0, int x1, int y1, unsigned char color) {
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int e2;

    while (1) {
        draw_dot(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_rect(int x, int y, int w, int h, unsigned char color) {
	draw_line(x, y, x+w-1, y, color);
	draw_line(x, y, x, y+h-1, color);
	draw_line(x, y+h-1, x+w-1, y+h-1, color);
	draw_line(x+w-1, y, x+w-1, y+h-1, color);
}

void fill_rect(int x, int y, int w, int h, unsigned char color) {
    uint8_t* buf = BACKBUF;
    for (int row = y; row < y + h; row++)
        memset(buf + row * 320 + x, color, w);
}

void draw_char(int x, int y, char c, unsigned char color, int size) { // 1 -  big, else small
    int offset, maxrow, maxcol;
    unsigned char startbit;
    if(size == 1) offset = (c - 32) * 8, maxrow = 8, maxcol = 8, startbit = 0x80;
    else offset = (c - 32) * 6, maxrow = 6, maxcol = 4, startbit = 0x08;;

    for (int row = 0; row < maxrow; row++) {
    	unsigned char row_data;
        if(size==1) row_data = font1_data[offset + row];
        else row_data = font2_data[offset + row];

        for (int col = 0; col < maxcol; col++) {
            if (row_data & (startbit >> col)) {
                draw_dot(x + col, y + row, color);
            }
        }
    }
}

void draw_string(int x, int y, char* str, unsigned char color, int size) {
    for (int i = 0; str[i] != '\0'; i++) {
    	int s = (size == 1) ? 8 : 5;
        draw_char(x + (i * s), y, str[i], color, size);
    }
}

void blit(void) {
    uint32_t* src = (uint32_t*)BACKBUF;
    uint32_t* dst = (uint32_t*)0xA0000;
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT / 4; i++)
        dst[i] = src[i];
}

void clear_screen(unsigned char color) {
    uint32_t* buf = (uint32_t*)BACKBUF;
    uint32_t  val = color | (color << 8) | (color << 16) | (color << 24);
    for (uint32_t i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT / 4; i++)
        buf[i] = val;
}

void delay(int count) {
    volatile int i;
    for (i = 0; i < count; i++)
        __asm__("nop");
}
