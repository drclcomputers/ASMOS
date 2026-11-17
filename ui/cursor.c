#include "ui/cursor.h"
#include "config/config.h"
#include "drivers/gpu.h"
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

/* ── save-under ────────────────────────────────────────────────────────
 * The back buffer is kept between frames (ui/window.h, lazy redraw), so
 * the pointer must be taken off it again: cursor_show() keeps the pixels
 * it covers, cursor_hide() puts them back. */
#define CUR_W 9
#define CUR_H 8
static uint8_t s_under[CUR_W * CUR_H];
static int s_ux, s_uy;
static bool s_shown;

void cursor_forget(void) { s_shown = false; } /* the frame was redrawn */

bool cursor_hide(int *y, int *h) {
    if (!s_shown)
        return false;
    s_shown = false;
    for (int r = 0; r < CUR_H; r++) {
        int py = s_uy + r;
        if (py < 0 || py >= SCREEN_HEIGHT)
            continue;
        for (int c = 0; c < CUR_W; c++) {
            int px = s_ux + c;
            if (px >= 0 && px < SCREEN_WIDTH)
                BACKBUF[py * SCREEN_WIDTH + px] = s_under[r * CUR_W + c];
        }
    }
    *y = s_uy;
    *h = CUR_H;
    return true;
}

/* the hourglass came up: the arrow last sent comes off the screen (the
 * back buffer still has it; the next frame's cursor_hide takes it off) */
void cursor_erase_screen(void) {
    if (s_shown)
        gpu_patch(s_ux, s_uy, CUR_W, CUR_H, s_under);
}

void cursor_show(int x, int y) {
    if (wd_busy_shown()) { /* the hourglass is the pointer now */
        s_shown = false;
        return;
    }
    for (int r = 0; r < CUR_H; r++) {
        int py = y + r;
        if (py < 0 || py >= SCREEN_HEIGHT)
            continue;
        for (int c = 0; c < CUR_W; c++) {
            int px = x + c;
            if (px >= 0 && px < SCREEN_WIDTH)
                s_under[r * CUR_W + c] = BACKBUF[py * SCREEN_WIDTH + px];
        }
    }
    s_ux = x;
    s_uy = y;
    s_shown = true;
    gfx_reset_clip();
    draw_cursor(x, y);
}
