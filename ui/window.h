#ifndef WINDOW_H
#define WINDOW_H

#include "lib/types.h"
#include "lib/string.h"
#include "ui/menubar.h"
#include "ui/widgets.h"
#include "config/config.h"

#define MAX_WINDOWS     16
#define MAX_WIN_WIDGETS 32
#define MAX_WIN_MENUS    8
#define TASKBAR_H       10

typedef struct window window;

struct window {
    int            x, y, w, h;
    char          *title;
    unsigned char  title_color;
    unsigned char  bar_color;
    unsigned char  content_color;
    bool           visible;
    bool           minimized;
    int            show_order;
    WidgetCallback on_close;
    WidgetCallback on_minimize;
    widget         widgets[MAX_WIN_WIDGETS];
    int            widget_count;
    menu           win_menus[MAX_WIN_MENUS];
    int            win_menu_count;
};

extern window *win_stack[MAX_WINDOWS];
extern int     win_count;

void   window_draw(window *win);
bool   window_update(window *win);
void   window_add_widget(window *win, widget wg);
void   window_dragged(window *win);
menu  *window_add_menu(window *win, char *title);

// window manager
void    wm_register(window *win);
void    wm_unregister(window *win);
void    wm_focus(window *win);
void    wm_draw_all(void);
void    wm_update_all(void);
window *wm_focused_window(void);
void    wm_sync_menubar(menubar *mb);

#endif
