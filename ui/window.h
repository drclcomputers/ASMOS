#ifndef WINDOW_H
#define WINDOW_H

#include "lib/core.h"
#include "lib/string.h"

#include "config/config.h"
#include "ui/menubar.h"
#include "ui/widgets.h"

#define TITLEBAR_H 16

#define MAX_WINDOWS 32
#define MAX_WIN_WIDGETS 128
#define MAX_WIN_MENUS 16
#define RESIZE_BUTTON 8

#define WIN_ANIM_NONE 0
#define WIN_ANIM_OPEN 1
#define WIN_ANIM_CLOSE 2
#define WIN_ANIM_MINIMIZE 3
#define WIN_ANIM_RESTORE 4

typedef struct window window;

typedef bool (*win_callback_t)(window *win);

typedef struct {
    int x, y;
    int w, h;
    int min_w, max_w;
    int min_h, max_h;
    bool resizable;
    const char *title;
    uint8_t title_color;
    uint8_t bar_color;
    uint8_t content_color;
    bool visible;
    bool pinned_bottom;
    win_callback_t on_close;
    win_callback_t on_minimize;
    void *app_instance;
} window_spec_t;

struct window {
    int x, y;
    int w, h;
    int min_w, max_w;
    int min_h, max_h;

    const char *title;
    uint8_t title_color;
    uint8_t bar_color;
    uint8_t content_color;

    bool visible;
    bool visible_buttons;
    bool minimized;
    bool dragging;
    bool resizing;
    bool resizable;
    int show_order;
    bool pinned_bottom;
    bool animate_open_close;
    uint8_t anim_state;
    uint8_t anim_frame;
    uint8_t anim_no_of_frames;

    win_callback_t on_close;
    win_callback_t on_minimize;

    void (*on_draw)(struct window *win, void *userdata);
    void *on_draw_userdata;

    widget widgets[MAX_WIN_WIDGETS];
    int widget_count;

    menu win_menus[MAX_WIN_MENUS];
    int win_menu_count;
    void *app_instance;
};

extern window *win_stack[MAX_WINDOWS];
extern int win_count;
extern window *focused_window;

bool is_desktop_focused();

window *wm_register(const window_spec_t *spec);

void wm_unregister(window *win);
void wm_focus(window *win);
window *wm_focused_window(void);
void wm_sync_menubar(menubar *mb);
void wm_draw_all(void);
void wm_update_all(void);

void window_add_widget(window *win, widget wg);
menu *window_add_menu(window *win, const char *title);
void window_draw(window *win);
bool window_update(window *win);
void window_resize(window *win);
void window_dragged(window *win);

#endif
