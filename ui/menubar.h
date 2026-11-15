#ifndef MENUBAR_H_
#define MENUBAR_H_

#include "lib/core.h"
#include "lib/string.h"

#define MAX_MENUS 8
#define MAX_MENU_ITEMS 12
#define MENU_ITEM_H 10
#define MENU_ITEM_MIN_W 60

typedef void (*MenuAction)(void);
typedef void (*MenuActionUD)(void *userdata);

typedef struct {
    char *label;
    MenuAction action;
    MenuActionUD action_ud;
    void *userdata;
    void *dyn_state; /* set for .APP items: called through the app guard */
    bool disabled;
} menu_item;

typedef struct {
    char *title;
    menu_item items[MAX_MENU_ITEMS];
    int item_count;
    bool open;
    int bar_x;
    int bar_w;
} menu;

typedef struct {
    menu menus[MAX_MENUS];
    int menu_count;
    int open_index;
} menubar;

extern menubar g_menubar;

void menubar_init(void);
menu *menubar_add_menu(menubar *mb, char *title);
void menu_add_item(menu *m, char *label, MenuAction action);
void menu_add_item_ud(menu *m, char *label, MenuActionUD action,
                      void *userdata);
void menu_add_separator(menu *m);
void menu_enable_last(menu *m, bool on); /* grey out the last item */
void menu_item_run(const menu_item *it); /* run its command */
/* set by the .APP loader: runs a dynamic app's menu command guarded */
extern void (*g_menu_call_hook)(void *state, MenuActionUD fn, void *user);
void menubar_layout(menubar *mb);
void menubar_draw(menubar *mb);
void menubar_update(menubar *mb);
void menubar_close_all(menubar *mb);

#endif
