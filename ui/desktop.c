#include "ui/desktop.h"
#include "ui/window.h"
#include "ui/menubar.h"
#include "os/os.h"
#include "config/config.h"
#include "lib/primitive_graphics.h"

#define WALLPAPER_SOLID         0
#define WALLPAPER_CHECKERBOARD  1
#define WALLPAPER_STRIPES       2
#define WALLPAPER_DOTS          3

#define PATTERN_MAIN_COLOR 		BLUE
#define PATTERN_SECONDARY_COLOR LIGHT_BLUE

static menu *apps_menu;

static void launch_finder(void)   { extern app_descriptor finder_app;   os_launch_app(&finder_app); }
static void launch_terminal(void) { extern app_descriptor terminal_app; os_launch_app(&terminal_app); }
static void launch_monitor(void)  { extern app_descriptor monitor_app;  os_launch_app(&monitor_app); }

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

void desktop_init(void) {
    static const window_spec_t spec = {
        .x = 0, .y = MENUBAR_H,
        .w = SCREEN_WIDTH,
        .h = SCREEN_HEIGHT - MENUBAR_H - TASKBAR_H,
        .title = "", .visible = true,
    };
    window *win = wm_register(&spec);
	if (!win) return;
	win->visible_buttons = false;
	win->pinned_bottom   = true;
	win->show_order      = -1;

    apps_menu = window_add_menu(win, "Apps");
    menu_add_item(apps_menu, "Finder",   launch_finder);
    menu_add_item(apps_menu, "Terminal", launch_terminal);
    menu_add_item(apps_menu, "Monitor",  launch_monitor);
}

void desktop_on_frame(void) {
    draw_wallpaper_pattern();
}
