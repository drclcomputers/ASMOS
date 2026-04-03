#ifndef WINDOW_H
#define WINDOW_H

#include "lib/types.h"
#include "lib/string.h"
#include "ui/menubar.h"
#include "ui/widgets.h"
#include "config/config.h"

#define MAX_WINDOWS 64
#define MAX_WIN_WIDGETS 128
#define MAX_WIN_MENUS 16

typedef struct window window;

typedef bool (*win_callback_t)(window *win);

typedef struct {
	int x, y;
	int w, h;
	const char *title;
	uint8_t title_color;
	uint8_t bar_color;
	uint8_t content_color;
	bool visible;
	win_callback_t on_close;
	win_callback_t on_minimize;
} window_spec_t;

struct window {
	int x, y;
	int w, h;

	const char *title;
	uint8_t title_color;
	uint8_t bar_color;
	uint8_t content_color;

	bool visible;
	bool minimized;
	bool dragging;
	int show_order;

	win_callback_t on_close;
	win_callback_t on_minimize;

	widget widgets[MAX_WIN_WIDGETS];
	int widget_count;

	menu win_menus[MAX_WIN_MENUS];
	int win_menu_count;
};

extern window *win_stack[MAX_WINDOWS];
extern int win_count;

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
void window_dragged(window *win);

#endif
