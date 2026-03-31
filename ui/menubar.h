#ifndef MENUBAR_H_
#define MENUBAR_H_

#include "lib/types.h"
#include "lib/string.h"

#define MAX_MENUS        8
#define MAX_MENU_ITEMS   12
#define MENU_ITEM_H      10
#define MENU_ITEM_MIN_W  60

typedef void (*MenuAction)(void);

typedef struct {
    char      *label;
    MenuAction action;
    bool       disabled;
} menu_item;

typedef struct {
    char      *title;
    menu_item  items[MAX_MENU_ITEMS];
    int        item_count;
    bool       open;
    int        bar_x;
    int        bar_w;
} menu;

typedef struct {
    menu menus[MAX_MENUS];
    int  menu_count;
    int  open_index;        // -1 = none open
} menubar;

extern menubar g_menubar;

void  menubar_init(void);
menu *menubar_add_menu(menubar *mb, char *title);
void  menu_add_item(menu *m, char *label, MenuAction action);
void  menu_add_separator(menu *m);
void  menubar_layout(menubar *mb);
void  menubar_draw(menubar *mb);
void  menubar_update(menubar *mb);
void  menubar_close_all(menubar *mb);

#endif
