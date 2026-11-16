#ifndef MENUBAR_H_
#define MENUBAR_H_

#include "lib/core.h"
#include "lib/string.h"

#define MAX_MENUS 16      /* a window's menus; the bar pages them (<< >>) */
#define MAX_MENU_ITEMS 16 /* items in a menu or submenu */
#define MENU_ITEM_H 10
#define MENU_ITEM_MIN_W 60

typedef void (*MenuAction)(void);
typedef void (*MenuActionUD)(void *userdata);

struct menu;

typedef struct {
    char *label;
    struct menu *sub; /* a submenu (an arrow on the right), or NULL */
    MenuAction action;
    MenuActionUD action_ud;
    void *userdata;
    void *dyn_state; /* set for .APP items: called through the app guard */
    bool disabled;
} menu_item;

typedef struct menu {
    char *title;
    menu_item items[MAX_MENU_ITEMS];
    int item_count;
    bool open;
    bool is_sub; /* a submenu: no submenus inside it (one level) */
    int bar_x;
    int bar_w;
} menu;

typedef struct {
    menu menus[MAX_MENUS];
    int menu_count;
    int open_index;
    int sub_item; /* item of the open menu whose submenu shows, or -1 */
    /* paging, when the menus don't fit across the screen */
    int first;                    /* first menu shown */
    int pages[MAX_MENUS], npages; /* where the earlier pages start (<<) */
    int prev_x, next_x;           /* the << and >> buttons, -1 = not shown */
    const void *owner; /* whose menus: paging resets for another window */
} menubar;

extern menubar g_menubar;

void menubar_init(void);
menu *menubar_add_menu(menubar *mb, char *title);
void menu_add_item(menu *m, char *label, MenuAction action);
void menu_add_item_ud(menu *m, char *label, MenuActionUD action,
                      void *userdata);
void menu_add_separator(menu *m);
/* An item that opens a submenu to the side (hover or click), like the
 * program groups of Windows. One level deep. The submenu is allocated;
 * menu_free_subs() frees those of a menu (windows do it when closed). */
menu *menu_add_submenu(menu *m, char *label);
void menu_free_subs(menu *m);
void menu_clear(menu *m);                /* no items (frees the submenus) */
void menu_enable_last(menu *m, bool on); /* grey out the last item */
void menu_item_run(const menu_item *it); /* run its command */
/* set by the .APP loader: runs a dynamic app's menu command guarded */
extern void (*g_menu_call_hook)(void *state, MenuActionUD fn, void *user);
void menubar_layout(menubar *mb);
void menubar_draw(menubar *mb);
void menubar_update(menubar *mb);
void menubar_close_all(menubar *mb);

void menubar_clock_text(char *out); /* 9 bytes */

#endif
