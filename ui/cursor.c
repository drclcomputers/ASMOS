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
#include "ui/menubar.h"
#include "ui/popup.h"

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

/* the bigger shapes: 'W' white, 'B' black, '.' see-through; the hot spot
 * in the middle */
typedef struct {
    const char *const *rows;
    int w, h;
} big_cursor_t;

static const char *const TEXT_ROWS[] = {
    /* 3: the I-beam, 5x11 */
    "WWWWW", "WBBBW", "WWBWW", ".WBW.", ".WBW.", ".WBW.",
    ".WBW.", ".WBW.", "WWBWW", "WBBBW", "WWWWW",
};
static const char *const MOVE_ROWS[] = {
    /* 4: four arrows, 13x13 */
    "......B......", ".....BWB.....", "....BWWWB....", "....BBWBB....",
    "..BB.BWB.BB..", ".BWBBBWBBBWB.", "BWWWWWWWWWWWB", ".BWBBBWBBBWB.",
    "..BB.BWB.BB..", "....BBWBB....", "....BWWWB....", ".....BWB.....",
    "......B......",
};
static const char *const NWSE_ROWS[] = {
    /* 5: top left - bottom right, 11x11 */
    "BBBBB......", "BWWWB......", "BWWB.......", "BWBWB......",
    "BB.BWB.....", "....BWB....", ".....BWB.BB", "......BWBWB",
    ".......BWWB", "......BWWWB", "......BBBBB",
};
static const char *const NESW_ROWS[] = {
    /* 6: top right - bottom left, 11x11 */
    "......BBBBB", "......BWWWB", ".......BWWB", "......BWBWB",
    ".....BWB.BB", "....BWB....", "BB.BWB.....", "BWBWB......",
    "BWWB.......", "BWWWB......", "BBBBB......",
};
static const char *const WE_ROWS[] = {
    /* 7: left - right, 13x7 */
    "...BB...BB...", "..BWB...BWB..", ".BWWBBBBBWWB.", "BWWWWWWWWWWWB",
    ".BWWBBBBBWWB.", "..BWB...BWB..", "...BB...BB...",
};
static const char *const NS_ROWS[] = {
    /* 8: up - down, 7x13 */
    "...B...", "..BWB..", ".BWWWB.", "BWWWWWB", "BBBWBBB", "..BWB..", "..BWB..",
    "..BWB..", "BBBWBBB", "BWWWWWB", ".BWWWB.", "..BWB..", "...B...",
};

#define BIG_FIRST CURSOR_TEXT
static const big_cursor_t BIG[] = {{TEXT_ROWS, 5, 11},  {MOVE_ROWS, 13, 13},
                                   {NWSE_ROWS, 11, 11}, {NESW_ROWS, 11, 11},
                                   {WE_ROWS, 13, 7},    {NS_ROWS, 7, 13}};
#define BIG_N (int)(sizeof(BIG) / sizeof(BIG[0]))
static uint8_t s_big[13 * 13]; /* the last one made */
static int s_big_made = -1;

/* any shape up on the screen (the hourglass's is the watchdog's) */
void cursor_put(int shape) {
    int k = shape - BIG_FIRST;
    if (k < 0 || k >= BIG_N) {
        pointer_shape(cursor_sprite(shape), CUR_W, CUR_H, 0, 0);
        return;
    }
    const big_cursor_t *b = &BIG[k];
    if (s_big_made != k) {
        for (int r = 0; r < b->h; r++)
            for (int c = 0; c < b->w; c++) {
                char ch = b->rows[r][c];
                s_big[r * b->w + c] = ch == 'W'   ? WHITE
                                      : ch == 'B' ? BLACK
                                                  : 0xFF;
            }
        s_big_made = k;
    }
    pointer_shape(s_big, b->w, b->h, b->w / 2, b->h / 2);
}

/* a shape for this frame (an app's, an edit field's), -1: none */
static int s_request = -1, s_shown = -1;

void cursor_request(int shape) {
    if (shape >= 0 && shape <= CURSOR_LAST)
        s_request = shape;
}

int cursor_shown(void) { return s_shown; }

void cursor_frame(bool on) {
    /* a menu open: the arrow, whatever is under it */
    bool menu = g_menubar.open_index >= 0 || popup_is_open();
    int shape = s_request >= 0 && !menu ? s_request : CHOSEN_CURSOR;
    s_request = -1; /* (asked for again next frame, if still wanted) */
    if (on && !wd_busy_shown()) { /* the hourglass stands in while busy */
        cursor_put(shape);
        s_shown = shape;
    }
    if (on && !ps2_irq_active()) /* else the timer interrupt moves it */
        pointer_move(mouse.x, mouse.y);
    pointer_show(on);
}

/* the hourglass gone: the shape put back (os/watchdog.c) */
void cursor_restore(void) { cursor_put(CHOSEN_CURSOR); }
