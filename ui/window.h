#ifndef WINDOW_H
#define WINDOW_H

#define MAX_WINDOWS      16
#define MAX_WIN_WIDGETS  32
#define MAX_WIN_MENUS     8
#define TASKBAR_H        10

#include "lib/types.h"
#include "lib/string.h"
#include "ui/widgets.h"

#define MAX_MENUS        8
#define MAX_MENU_ITEMS   12
#define MENU_ITEM_H      10
#define MENU_ITEM_MIN_W  60
#define MENUBAR_H_SIZE   10

typedef void (*MenuAction)(void);

typedef struct {
    char       *label;
    MenuAction  action;
    bool        disabled;
} menu_item;

typedef struct {
    char       *title;
    menu_item   items[MAX_MENU_ITEMS];
    int         item_count;
    bool        open;
    int         bar_x;
    int         bar_w;
} menu;

typedef struct {
    menu menus[MAX_MENUS];
    int  menu_count;
    int  open_index;
} menubar;

extern menubar g_menubar;

void menubar_init(void);
menu *menubar_add_menu(menubar *mb, char *title);
void  menu_add_item(menu *m, char *label, MenuAction action);
void  menu_add_separator(menu *m);
void  menubar_draw(menubar *mb);
void menubar_layout(menubar *mb);
void  menubar_update(menubar *mb);
void  menubar_close_all(menubar *mb);


typedef struct window window;

struct window {
    int x, y, w, h;
    char          *title;
    unsigned char  title_color;
    unsigned char  bar_color;
    unsigned char  content_color;
    bool           visible;
    bool           minimized;
    int            show_order;

    WidgetCallback on_close;
    WidgetCallback on_minimize;

    widget widgets[MAX_WIN_WIDGETS];
    int    widget_count;

    menu   win_menus[MAX_WIN_MENUS];
    int    win_menu_count;
};

extern window *win_stack[MAX_WINDOWS];
extern int     win_count;

void   window_draw(window *win);
bool   window_update(window *win);
void   window_add_widget(window *win, widget wg);
void   window_dragged(window *win);
menu  *window_add_menu(window *win, char *title);

void    wm_register(window *win);
void    wm_unregister(window *win);
void    wm_focus(window *win);
void    wm_draw_all(void);
void    wm_update_all(void);
window *wm_focused_window(void);
void    wm_sync_menubar(menubar *mb);

#endif
