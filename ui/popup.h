#ifndef POPUP_H_
#define POPUP_H_

/*
 * Context (popup) menus. One popup is open at a time.
 *
 *   menu *m = popup_menu();               // empty shared menu
 *   menu_add_item_ud(m, "Copy", do_copy, s);
 *   menu_enable_last(m, has_selection);   // grey out
 *   menu_add_separator(m);
 *   popup_show_for(c);                    // at the right-click point
 *
 * Labels are kept as pointers: pass string literals or static buffers.
 * Controls get EV_MENU on right-click, Shift+F10 or the Menu key; c->mx/my
 * hold the point (lists and trees select the row under it first).
 */

#include "ui/controls.h"
#include "ui/menubar.h"

menu *popup_menu(void);
void popup_show(int x, int y); /* screen position, kept on screen */
void popup_show_for(const control *c);
bool popup_is_open(void);
/* the window the popup belongs to closes: its actions' data goes too */
void popup_window_gone(const window *w);
void popup_close(void);

/* main loop: update before the menubar/controls see input, draw last */
void popup_update(void);
void popup_draw(void);

#endif
