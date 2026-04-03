#include "ui/ui.h"
#include "lib/primitive_graphics.h"
#include "config/config.h"

#define WALLPAPER_SOLID         0
#define WALLPAPER_CHECKERBOARD  1
#define WALLPAPER_STRIPES       2
#define WALLPAPER_DOTS          3

#define PATTERN_MAIN_COLOR 		BLUE
#define PATTERN_SECONDARY_COLOR LIGHT_BLUE

void draw_wallpaper_pattern() {
    switch (WALLPAPER_PATTERN) {
        case WALLPAPER_SOLID:
            clear_screen(PATTERN_MAIN_COLOR);
            break;

        case WALLPAPER_CHECKERBOARD:
            for (int y = 0; y < SCREEN_HEIGHT; y += 10) {
                for (int x = 0; x < SCREEN_WIDTH; x += 10) {
                    unsigned char color = ((x / 10) + (y / 10)) % 2 == 0 ? PATTERN_MAIN_COLOR : PATTERN_SECONDARY_COLOR;
                    fill_rect(x, y, 10, 10, color);
                }
            }
            break;

        case WALLPAPER_STRIPES:
            for (int y = 0; y < SCREEN_HEIGHT; y++) {
                unsigned char color = (y / 5) % 2 == 0 ? PATTERN_MAIN_COLOR : PATTERN_SECONDARY_COLOR;
                draw_line(0, y, SCREEN_WIDTH - 1, y, color);
            }
            break;

        case WALLPAPER_DOTS:
            clear_screen(PATTERN_MAIN_COLOR);
            for (int y = 5; y < SCREEN_HEIGHT; y += 10) {
                for (int x = 5; x < SCREEN_WIDTH; x += 10) {
                    draw_dot(x, y, PATTERN_SECONDARY_COLOR);
                }
            }
            break;

        default:
            clear_screen(WHITE);
            break;
    }
}
