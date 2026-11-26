// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "ui/cursor.h"
#include "config/config.h"
#include "drivers/mouse.h"
#include "drivers/pointer.h"
#include "drivers/ps2.h"
#include "lib/core.h"
#include "lib/graphics.h"
#include "os/watchdog.h"

#define ARROW_CURSOR 0
#define POINTER_CURSOR 1
#define CROSSHAIR_CURSOR 2
#define TEXT_CURSOR 3

int CHOSEN_CURSOR = 0;

void cursortype(int chosencursor) { CHOSEN_CURSOR = chosencursor; }

static const uint16_t cursor_fill[4][8] = {
    {// 0: ARROW_CURSOR
     0b110000000, 0b111000000, 0b111100000, 0b111110000, 0b111111000,
     0b111111000, 0b001110000, 0b000110000},
    {// 1: POINTER_CURSOR
     0b011100000, 0b011100000, 0b011111110, 0b111111111, 0b111111111,
     0b111111111, 0b011111110, 0b011111110},
    {// 2: CROSSHAIR_CURSOR
     0b000010000, 0b000010000, 0b000010000, 0b011111110, 0b000010000,
     0b000010000, 0b000010000, 0b000000000},
    {// 3: TEXT_CURSOR
     0b000000000, 0b000010000, 0b000010000, 0b000010000, 0b000010000,
     0b000010000, 0b000010000, 0b000000000}};

static const uint16_t cursor_border[4][8] = {
    {// 0: ARROW_CURSOR
     0b110000000, 0b101000000, 0b100100000, 0b100010000, 0b100001000,
     0b110001000, 0b001010000, 0b000110000},
    {// 1: POINTER_CURSOR
     0b011100000, 0b010100000, 0b010111110, 0b100010101, 0b100000001,
     0b100000001, 0b010000010, 0b011111110},
    {// 2: CROSSHAIR_CURSOR
     0b000000000, 0b000010000, 0b000010000, 0b001101100, 0b000010000,
     0b000010000, 0b000000000, 0b000000000},
    {// 3: TEXT_CURSOR
     0b000111000, 0b000000000, 0b000000000, 0b000000000, 0b000000000,
     0b000000000, 0b000000000, 0b000111000}};

void draw_cursor(int x, int y) {
    if (CHOSEN_CURSOR < 0 || CHOSEN_CURSOR > 3)
        CHOSEN_CURSOR = 0;
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

// the pointer on the screen (drivers/pointer.h)
/*
 * The pointer is not drawn into the frame any more: it goes straight onto
 * the screen and the timer interrupt moves it. These are the shapes. */
#define CUR_W 9
#define CUR_H 8
static uint8_t s_sprite[4][CUR_W * CUR_H];
static bool s_made;

const uint8_t *cursor_sprite(int shape) {
    if (shape < 0 || shape > 3)
        shape = 0;
    if (!s_made) {
        for (int k = 0; k < 4; k++)
            for (int row = 0; row < CUR_H; row++)
                for (int col = 0; col < CUR_W; col++) {
                    uint16_t bit = (uint16_t)(1 << (8 - col));
                    uint8_t c = 0xFF;
                    if (cursor_fill[k][row] & bit)
                        c = WHITE;
                    if (cursor_border[k][row] & bit)
                        c = BLACK;
                    s_sprite[k][row * CUR_W + col] = c;
                }
        s_made = true;
    }
    return s_sprite[shape];
}

/* the move pointer (4): four arrows, 17x17, the hot spot in the middle.
 * 'W' white, 'B' black */
#define MOVE_N 17
static const char *const MOVE_ROWS[MOVE_N] = {
    "........B........", ".......BWB.......", "......BWWWB......",
    ".....BWWWWWB.....", "......BBWBB......", "...B...BWB...B...",
    "..BWB..BWB..BWB..", ".BWWBBBBWBBBBWWB.", "BWWWWWWWWWWWWWWWB",
    ".BWWBBBBWBBBBWWB.", "..BWB..BWB..BWB..", "...B...BWB...B...",
    "......BBWBB......", ".....BWWWWWB.....", "......BWWWB......",
    ".......BWB.......", "........B........",
};
static uint8_t s_move[MOVE_N * MOVE_N];

static const uint8_t *move_sprite(void) {
    if (!s_move[0] && !s_move[1]) { /* (made once: corners are see-through) */
        for (int r = 0; r < MOVE_N; r++)
            for (int c = 0; c < MOVE_N; c++) {
                char ch = MOVE_ROWS[r][c];
                s_move[r * MOVE_N + c] = ch == 'W'   ? WHITE
                                         : ch == 'B' ? BLACK
                                                     : 0xFF;
            }
    }
    return s_move;
}

/* an app's shape for this frame (API size 12), -1: none */
static int s_request = -1, s_shown;

void cursor_request(int shape) {
    if (shape >= 0 && shape <= CURSOR_MOVE)
        s_request = shape;
}

int cursor_shown(void) { return s_shown; }

void cursor_frame(bool on) {
    int shape = s_request >= 0 ? s_request : CHOSEN_CURSOR;
    s_request = -1; /* (asked for again next frame, if still wanted) */
    if (on && !wd_busy_shown()) { /* the hourglass stands in while busy */
        if (shape == CURSOR_MOVE)
            pointer_shape(move_sprite(), MOVE_N, MOVE_N, MOVE_N / 2,
                          MOVE_N / 2);
        else
            pointer_shape(cursor_sprite(shape), CUR_W, CUR_H, 0, 0);
        s_shown = shape;
    }
    if (on && !ps2_irq_active()) /* else the timer interrupt moves it */
        pointer_move(mouse.x, mouse.y);
    pointer_show(on);
}
