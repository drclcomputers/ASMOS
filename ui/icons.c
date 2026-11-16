#include "ui/icons.h"

#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"

#include "config/config.h"

// Icon types img
void draw_file_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax + 1, ay, ICON_W - 5, ICON_H, bg);
    draw_rect(ax + 1, ay, ICON_W - 5, ICON_H, BLACK);
    fill_rect(ax + ICON_W - 5, ay, 4, ICON_H, BLACK);
    draw_line(ax + ICON_W - 5, ay, ax + ICON_W - 1, ay + 4, BLACK);
    fill_rect(ax + ICON_W - 4, ay, 3, 4, bg);
    if (!sel) {
        draw_line(ax + 3, ay + 5, ax + ICON_W - 6, ay + 5, DARK_GRAY);
        draw_line(ax + 3, ay + 8, ax + ICON_W - 6, ay + 8, DARK_GRAY);
        draw_line(ax + 3, ay + 11, ax + ICON_W - 6, ay + 11, DARK_GRAY);
    }
}

void draw_folder_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax, ay + 3, ICON_W, ICON_H - 3, bg);
    draw_rect(ax, ay + 3, ICON_W, ICON_H - 3, BLACK);
    fill_rect(ax + 1, ay, 8, 4, bg);
    draw_rect(ax + 1, ay, 8, 4, BLACK);
    draw_line(ax, ay + 3, ax + 1, ay, BLACK);
    draw_line(ax + 9, ay, ax + 9, ay + 3, BLACK);
    if (sel)
        fill_rect(ax + 2, ay + 5, ICON_W - 4, ICON_H - 9, BLACK);
}

void draw_app_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? LIGHT_BLUE : WHITE;
    uint8_t title = sel ? DARK_GRAY : BLUE;
    uint8_t content = sel ? WHITE : DARK_GRAY;

    fill_rect(ax + 1, ay + 1, ICON_W - 2, ICON_H - 2, bg);
    draw_rect(ax + 1, ay + 1, ICON_W - 2, ICON_H - 2, BLACK);

    fill_rect(ax + 2, ay + 2, ICON_W - 4, 3, title);

    draw_line(ax + 3, ay + 7, ax + ICON_W - 4, ay + 7, content);
    draw_line(ax + 3, ay + 10, ax + ICON_W - 4, ay + 10, content);
    draw_line(ax + 3, ay + 13, ax + ICON_W - 4, ay + 13, content);
}

void draw_dotdot_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : LIGHT_GRAY;
    fill_rect(ax, ay + 3, ICON_W, ICON_H - 3, bg);
    draw_rect(ax, ay + 3, ICON_W, ICON_H - 3, BLACK);
    draw_string(ax + 4, ay + 8, "..", BLACK, 2);
}

void draw_pic_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax + 1, ay, ICON_W - 5, ICON_H, bg);
    draw_rect(ax + 1, ay, ICON_W - 5, ICON_H, BLACK);
    fill_rect(ax + ICON_W - 5, ay, 4, ICON_H, BLACK);
    draw_line(ax + ICON_W - 5, ay, ax + ICON_W - 1, ay + 4, BLACK);
    fill_rect(ax + ICON_W - 4, ay, 3, 4, bg);
    fill_rect(ax + 3, ay + 3, ICON_W - 8, 5, sel ? DARK_GRAY : LIGHT_BLUE);
    fill_rect(ax + 3, ay + 8, ICON_W - 8, 4, sel ? DARK_GRAY : LIGHT_GREEN);
    draw_dot(ax + 4, ay + 4, sel ? WHITE : YELLOW);
}

void draw_txt_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax + 1, ay, ICON_W - 5, ICON_H, bg);
    draw_rect(ax + 1, ay, ICON_W - 5, ICON_H, BLACK);
    fill_rect(ax + ICON_W - 5, ay, 4, ICON_H, BLACK);
    draw_line(ax + ICON_W - 5, ay, ax + ICON_W - 1, ay + 4, BLACK);
    fill_rect(ax + ICON_W - 4, ay, 3, 4, bg);
    uint8_t line_col = sel ? WHITE : BLACK;
    draw_line(ax + 3, ay + 4, ax + ICON_W - 7, ay + 4, line_col);
    draw_line(ax + 3, ay + 7, ax + ICON_W - 7, ay + 7, line_col);
    draw_line(ax + 3, ay + 10, ax + ICON_W - 7, ay + 10, line_col);
    draw_line(ax + 3, ay + 13, ax + ICON_W - 9, ay + 13, line_col);
}

/* a page with a zip fastener down the middle, open at the top */
void draw_zip_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : YELLOW;
    fill_rect(ax + 1, ay, ICON_W - 5, ICON_H, bg);
    draw_rect(ax + 1, ay, ICON_W - 5, ICON_H, BLACK);
    fill_rect(ax + ICON_W - 5, ay, 4, ICON_H, BLACK);
    draw_line(ax + ICON_W - 5, ay, ax + ICON_W - 1, ay + 4, BLACK);
    fill_rect(ax + ICON_W - 4, ay, 3, 4, bg);
    int cx = ax + 1 + (ICON_W - 5) / 2; /* the middle of the page */
    uint8_t tooth = sel ? WHITE : DARK_GRAY;
    uint8_t tape = sel ? LIGHT_GRAY : BROWN;
    /* the tapes either side, then the teeth meeting between them */
    draw_line(cx - 3, ay + 1, cx - 3, ay + ICON_H - 2, tape);
    draw_line(cx + 2, ay + 1, cx + 2, ay + ICON_H - 2, tape);
    for (int y = ay + 2; y < ay + 8; y += 2) { /* open: apart */
        draw_line(cx - 2, y, cx - 2, y, tooth);
        draw_line(cx + 1, y + 1, cx + 1, y + 1, tooth);
    }
    for (int y = ay + 13; y < ay + ICON_H - 1; y += 2) { /* closed */
        draw_line(cx - 2, y, cx, y, tooth);
        draw_line(cx - 1, y + 1, cx + 1, y + 1, tooth);
    }
    /* the slider, a shine on it */
    fill_rect(cx - 2, ay + 9, 4, 4, BLACK);
    draw_dot(cx - 1, ay + 10, sel ? WHITE : LIGHT_GRAY);
}

void draw_cfg_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : LIGHT_GRAY;
    fill_rect(ax + 1, ay, ICON_W - 5, ICON_H, bg);
    draw_rect(ax + 1, ay, ICON_W - 5, ICON_H, BLACK);
    fill_rect(ax + ICON_W - 5, ay, 4, ICON_H, BLACK);
    draw_line(ax + ICON_W - 5, ay, ax + ICON_W - 1, ay + 4, BLACK);
    fill_rect(ax + ICON_W - 4, ay, 3, 4, bg);
    int cx = ax + (ICON_W - 5) / 2 + 1;
    int cy = ay + ICON_H / 2 + 1;
    uint8_t gc = sel ? WHITE : DARK_GRAY;
    draw_line(cx - 1, cy - 4, cx + 1, cy - 4, gc);
    draw_line(cx - 1, cy + 3, cx + 1, cy + 3, gc);
    draw_line(cx - 4, cy - 1, cx - 4, cy + 1, gc);
    draw_line(cx + 3, cy - 1, cx + 3, cy + 1, gc);
    fill_rect(cx - 2, cy - 2, 5, 5, gc);
    fill_rect(cx - 1, cy - 1, 3, 3, bg);
}

void draw_hdd_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : LIGHT_GRAY;
    uint8_t plat = sel ? LIGHT_GRAY : WHITE;
    /* box */
    fill_rect(ax + 1, ay + 2, ICON_W - 2, ICON_H - 4, bg);
    draw_rect(ax + 1, ay + 2, ICON_W - 2, ICON_H - 4, BLACK);
    /* platters */
    fill_rect(ax + 4, ay + 5, ICON_W - 8, 4, plat);
    draw_rect(ax + 4, ay + 5, ICON_W - 8, 4, DARK_GRAY);
    fill_rect(ax + 4, ay + 11, ICON_W - 8, 4, plat);
    draw_rect(ax + 4, ay + 11, ICON_W - 8, 4, DARK_GRAY);
    /* LED */
    fill_rect(ax + ICON_W - 6, ay + 3, 3, 3, sel ? DARK_GRAY : LIGHT_GREEN);
}

void draw_floppy_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : LIGHT_GRAY;
    fill_rect(ax + 1, ay, ICON_W - 2, ICON_H, bg);
    draw_rect(ax + 1, ay, ICON_W - 2, ICON_H, BLACK);
    // shutter slot
    fill_rect(ax + 3, ay + ICON_H - 6, ICON_W - 6, 5,
              sel ? LIGHT_GRAY : DARK_GRAY);
    draw_rect(ax + 3, ay + ICON_H - 6, ICON_W - 6, 5, BLACK);
    // label area
    fill_rect(ax + 3, ay + 2, ICON_W - 10, 6, sel ? DARK_GRAY : WHITE);
    draw_rect(ax + 3, ay + 2, ICON_W - 10, 6, BLACK);
}

void draw_unknown_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax + 1, ay, ICON_W - 5, ICON_H, bg);
    draw_rect(ax + 1, ay, ICON_W - 5, ICON_H, BLACK);
    fill_rect(ax + ICON_W - 5, ay, 4, ICON_H, BLACK);
    draw_line(ax + ICON_W - 5, ay, ax + ICON_W - 1, ay + 4, BLACK);
    fill_rect(ax + ICON_W - 4, ay, 3, 4, bg);
    draw_string(ax + 5, ay + 7, "?", sel ? WHITE : DARK_GRAY, 2);
}

/* a dustbin with a lid; full: paper sticking out */
void draw_trash_icon(int ax, int ay, bool sel, bool full) {
    uint8_t body = sel ? DARK_GRAY : LIGHT_GRAY;
    uint8_t rib = sel ? LIGHT_GRAY : DARK_GRAY;
    if (full) { /* crumpled paper above the rim */
        fill_rect(ax + 5, ay, 5, 4, WHITE);
        draw_rect(ax + 5, ay, 5, 4, BLACK);
        fill_rect(ax + 10, ay + 1, 5, 3, WHITE);
        draw_rect(ax + 10, ay + 1, 5, 3, BLACK);
    }
    fill_rect(ax + 2, ay + 4, ICON_W - 4, 3, body); /* lid */
    draw_rect(ax + 2, ay + 4, ICON_W - 4, 3, BLACK);
    fill_rect(ax + 8, ay + 2 + (full ? 0 : 1), 4, 2, BLACK); /* handle */
    fill_rect(ax + 4, ay + 7, ICON_W - 8, ICON_H - 7, body); /* can */
    draw_rect(ax + 4, ay + 7, ICON_W - 8, ICON_H - 7, BLACK);
    for (int x = ax + 7; x < ax + ICON_W - 6; x += 3)
        draw_line(x, ay + 9, x, ay + ICON_H - 3, rib);
}
