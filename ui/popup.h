#ifndef POPUP_H_
#define POPUP_H_

/*
 * Build the shared popup_menu(), then show it with popup_show_for(c).
 * Labels must outlive the menu. Controls send EV_MENU on right-click,
 * Shift+F10, or Menu; lists and trees select the pointed row first.
 */

#include "ui/controls.h"
#include "ui/menubar.h"

menu *popup_menu(void);
bool popup_is_menu(const menu *m); /* the context menu (no submenus) */
void popup_show(int x, int y);     /* screen position, kept on screen */
void popup_show_for(const control *c);
bool popup_is_open(void);
/* the window the popup belongs to closes: its actions' data goes too */
void popup_window_gone(const window *w);
void popup_close(void);

/* main loop: update before the menubar/controls see input, draw last */
void popup_update(void);
void popup_draw(void);

#endif
