// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef WINDOW_H
#define WINDOW_H

#include "lib/core.h"
#include "lib/string.h"

#include "config/config.h"
#include "ui/controls.h"
#include "ui/menubar.h"

#define TITLEBAR_H 16

#define MAX_WINDOWS 32
#define MAX_WIN_MENUS 16
#define RESIZE_BUTTON 10

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

    menu win_menus[MAX_WIN_MENUS];
    int win_menu_count;
    void *app_instance;

    /* UI toolkit (ui/controls.h) */
    control *controls;          /* creation order = tab order = paint order */
    control *focus;             /* control with keyboard focus             */
    void *userdata;             /* app state; default control->user        */
    void *dyn_state;            /* set for windows of dynamic .APP apps    */
    struct window *modal_child; /* open dialog blocking this window   */
    struct window *modal_owner; /* for dialogs: the window they block */
    bool fit_pending; /* shrink/move onto the screen before first draw */

    /* WindowShade: double-click the title bar to roll up to it */
    bool shaded;
    uint32_t title_click_t; /* last click on the title bar (ms) */
    /* wm_zoom(): toggles between the user's size and the whole desktop;
     * zx.. is the size to go back to (no title-bar button) */
    bool zoomed;
    int zx, zy, zw, zh;

    /* lazy redraw (see wm_compose): */
    bool dirty;       /* wm_invalidate(): draw it on the next frame        */
    bool lazy_custom; /* custom-drawn content only changes in handlers or
                         with wm_invalidate(); else it is drawn every frame */
    uint32_t sig;     /* what was last drawn (title, controls)             */

    /* outline move/resize (Settings > Performance): the window stays put
     * and a frame shows where it will go; applied on release */
    bool band;
    int bx, by, bw, bh;

    /* an app's main window comes back where it was last closed: the app's
     * name (the key in /PREFS/WINDOWS.INI) and where it opened ("" = not
     * remembered: dialogs, later windows, AW_FIXED_POS) */
    char pos_key[24];
    int pos_x0, pos_y0;
};

/* height the window takes on screen: just the title bar when shaded */
static inline int win_shown_h(const window *w) {
    return w->shaded ? TITLEBAR_H + 1 : w->h;
}

extern window *win_stack[MAX_WINDOWS];
extern int win_count;
extern window *focused_window;

bool is_desktop_focused();
/* the window this frame's click went down on (wm_update_all looks before a
 * title bar button closes or minimizes it), NULL if none or no click: the
 * desktop takes a click only if it is its own */
window *wm_click_window(void);
/* Alt+Tab / Alt+Shift+Tab: called once a frame before the keys go
 * anywhere else; true while the window list is up (it took the key) */
bool wm_switcher_key(void);
bool wm_switcher_active(void);

void wm_init(void);
window *wm_register(const window_spec_t *spec);

void wm_unregister(window *win);
/* Remembered positions (os_launch_app): the first app_window() an app makes
 * while starting gets `name`'s saved place. NULL after its init. */
void wm_set_launch_key(const char *name);
const char *wm_take_launch_key(void);
/* where `name`'s window was last; false = never saved */
bool wm_saved_pos(const char *name, int *x, int *y);
void wm_save_positions(void); /* every open one (Shut Down / Restart) */
void wm_focus(window *win);
window *wm_focused_window(void);
void wm_sync_menubar(menubar *mb);
void wm_draw_all(void);

// lazy redraw
/*
 * Redraws changed windows and their overlaps; an idle desktop draws nothing.
 * Controls are tracked automatically. Custom-drawn windows redraw each frame
 * unless lazy_custom is set and wm_invalidate() marks external changes.
 */
void wm_invalidate(window *win);     /* redraw this window next frame */
void wm_invalidate_all(void);        /* redraw the whole screen next frame */
void wm_invalidate_app(window *win); /* all windows of win's app */
void wm_frame_begin(void);           /* main loop, right after input is read */
/* Draw the frame. Returns WM_FRAME_*; for PART, rows *y0..*y1 changed. */
enum { WM_FRAME_NONE, WM_FRAME_PART, WM_FRAME_FULL };
int wm_compose(menubar *mb, int *y0, int *y1);
bool wm_debug_reference(menubar *mb); /* tests: plain full redraw */
extern uint32_t g_wm_frames[3];       /* frames of each WM_FRAME_* kind */
void wm_update_all(void);

bool wm_is_registered(const window *win);
void wm_close(window *win); /* what the close button does */
menu *window_add_menu(window *win, const char *title);
/* Draw fn(user) on top of every window each frame (one at a time), e.g.
 * an icon dragged across windows. Clear it with the same user pointer. */
void wm_set_overlay(void (*fn)(void *user), void *user);
void wm_clear_overlay(void *user);
void window_draw(window *win);
bool window_update(window *win);
void window_resize(window *win);
void window_dragged(window *win);
void wm_shade(window *win, bool shaded);
void wm_zoom(window *win); /* fill the desktop / restore */

#endif
