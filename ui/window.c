#include "ui/window.h"
#include "lib/utils.h"

void draw_window(int x, int y, int w, int h, char* title) {
    // title bar
    fill_rect(x, y, w, 16, 0x08);

    // close box
    fill_rect(x + 3, y + 3, 10, 10, 0xCC);
    draw_rect(x + 3, y + 3, 10, 10, 0x00);

    // title text
    int title_x = x + w/2 - (strlen(title) * 3);  // approx center for small font
    draw_string(title_x, y + 6, title, 0xFF, 2);

    // Content area
    fill_rect(x, y + 16, w, h - 16, 0xEE);

    // Border
    draw_rect(x, y, w, h, 0x00);
}
