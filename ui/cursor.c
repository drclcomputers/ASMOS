#include "ui/cursor.h"
#include "lib/primitive_graphics.h"
#include "lib/types.h"
#include "config/config.h"

static const uint16_t cursor_fill[8] = {
    0b110000000,
    0b111000000,
    0b111100000,
    0b111110000,
    0b111111000,
    0b111111000,
    0b001110000,
    0b000110000
};

static const uint16_t cursor_border[8] = {
    0b110000000,
    0b101000000,
    0b100100000,
    0b100010000,
    0b100001000,
    0b110001000,
    0b001010000,
    0b000110000
};

void draw_cursor(int x, int y) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            int py = y + row;

            if (px < 0 || px >= SCREEN_WIDTH || py < 0 || py >= SCREEN_HEIGHT)
                continue;

            // fill
            if (cursor_fill[row] & (1 << (8 - col))) {
                draw_dot(px, py, WHITE);
            }

            // border
            if (cursor_border[row] & (1 << (8 - col))) {
                draw_dot(px, py, BLACK);
            }
        }
    }
}
