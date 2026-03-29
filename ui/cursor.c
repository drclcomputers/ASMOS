#include "cursor.h"
#include "lib/primitive_graphics.h"
#include "lib/types.h"

static const uint8_t cursor_shape[8] = {
    0b11000000,
    0b11100000,
    0b11110000,
    0b11111000,
    0b11110000,
    0b10110000,
    0b00011000,
    0b00011000
};

void draw_cursor(int x, int y) {
    for (int row = 0; row < 11; row++) {
        for (int col = 0; col < 16; col++) {
            if (cursor_shape[row] & (1 << (10 - col))) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < 320 && py >= 0 && py < 200)
                    draw_dot(px, py, 0x00);
            }
        }
    }
}
