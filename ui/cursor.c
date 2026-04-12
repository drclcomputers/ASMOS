#include "ui/cursor.h"
#include "lib/graphics.h"
#include "lib/core.h"
#include "config/config.h"

#define ARROW_CURSOR     0
#define POINTER_CURSOR   1
#define CROSSHAIR_CURSOR 2
#define TEXT_CURSOR      3

int CHOSEN_CURSOR = 0;

static const uint16_t cursor_fill[4][8] = {
    {   // 0: ARROW_CURSOR
        0b110000000,
        0b111000000,
        0b111100000,
        0b111110000,
        0b111111000,
        0b111111000,
        0b001110000,
        0b000110000
    },
    {   // 1: POINTER_CURSOR
        0b011100000,
        0b011100000,
        0b011111110,
        0b111111111,
        0b111111111,
        0b111111111,
        0b011111110,
        0b011111110
    },
    {   // 2: CROSSHAIR_CURSOR
        0b000010000,
        0b000010000,
        0b000010000,
        0b011111110,
        0b000010000,
        0b000010000,
        0b000010000,
        0b000000000
    },
    {   // 3: TEXT_CURSOR
        0b000000000,
        0b000010000,
        0b000010000,
        0b000010000,
        0b000010000,
        0b000010000,
        0b000010000,
        0b000000000
    }
};

static const uint16_t cursor_border[4][8] = {
    {   // 0: ARROW_CURSOR
        0b110000000,
        0b101000000,
        0b100100000,
        0b100010000,
        0b100001000,
        0b110001000,
        0b001010000,
        0b000110000
    },
    {   // 1: POINTER_CURSOR
    	0b011100000,
        0b010100000,
        0b010111110,
        0b100010101,
        0b100000001,
        0b100000001,
        0b010000010,
        0b011111110
    },
    {   // 2: CROSSHAIR_CURSOR
        0b000000000,
        0b000010000,
        0b000010000,
        0b001101100,
        0b000010000,
        0b000010000,
        0b000000000,
        0b000000000
    },
    {   // 3: TEXT_CURSOR
        0b000111000,
        0b000000000,
        0b000000000,
        0b000000000,
        0b000000000,
        0b000000000,
        0b000000000,
        0b000111000
    }
};

void draw_cursor(int x, int y) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 9; col++) {
            int px = x + col;
            int py = y + row;

            if (px < 0 || px >= SCREEN_WIDTH || py < 0 || py >= SCREEN_HEIGHT)
                continue;

            // fill
            if (cursor_fill[CHOSEN_CURSOR][row] & (1 << (8 - col))) {
                draw_dot(px, py, WHITE);
            }

            // border
            if (cursor_border[CHOSEN_CURSOR][row] & (1 << (8 - col))) {
                draw_dot(px, py, BLACK);
            }
        }
    }
}
