#include "ui/window.h"
#include "ui/menubar.h"
#include "lib/alloc.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "io/mouse.h"
#include "config/config.h"
#include "lib/primitive_graphics.h"

window *win_stack[MAX_WINDOWS];
int     win_count = 0;

static void wm_sort(void) {
    for (int i = 0; i < win_count - 1; i++) {
        for (int j = i + 1; j < win_count; j++) {
            if (win_stack[i]->show_order > win_stack[j]->show_order) {
                window *tmp  = win_stack[i];
                win_stack[i] = win_stack[j];
                win_stack[j] = tmp;
            }
        }
    }
}

static void wm_clamp(window *win) {
    if (win->x < 0) win->x = 0;
    if (win->y < 0) win->y = 0;
    if (win->x + win->w > SCREEN_WIDTH) win->x = SCREEN_WIDTH - win->w;
    if (win->y + win->h + MENUBAR_H_SIZE + TASKBAR_H > SCREEN_HEIGHT) win->y = SCREEN_HEIGHT - win->h - MENUBAR_H_SIZE - TASKBAR_H;
}

static bool in_titlebar(const window *win, int mx, int my) {
    int wy = win->y + MENUBAR_H_SIZE;
    return mx >= win->x && mx < win->x + win->w && my >= wy && my <  wy + 16;
}

static void draw_titlebar_btn(int ax, int ay, int w, int h, const char *label, uint8_t bg) {
    fill_rect(ax, ay, w, h, bg);
    draw_rect(ax, ay, w, h, BLACK);
    int tx = ax + w / 2 - (int)(strlen(label) * 3) + 1;
    int ty = ay + h / 2 - 3 + 1;
    draw_string(tx, ty, (char *)label, WHITE, 2);
}

static bool clicked_titlebar_btn(int ax, int ay, int w, int h) {
    return mouse.left_clicked && mouse.x >= ax && mouse.x < ax + w && mouse.y >= ay && mouse.y < ay + h;
}

static int taskbar_label_width(const window *win) {
    return (int)strlen(win->title) * 5 + 8;
}

window *wm_register(const window_spec_t *spec) {
    if (!spec) return NULL;
    if (win_count >= MAX_WINDOWS) return NULL;
    if (spec->w <= 0 || spec->h <= 0) return NULL;

    window *win = (window *)kmalloc(sizeof(window));
    if (!win) return NULL;

    memset(win, 0, sizeof(window));

    win->x               = spec->x;
    win->y               = spec->y;
    win->w               = spec->w;
    win->h               = spec->h;
    win->title           = spec->title ? spec->title : "";
    win->title_color     = spec->title_color;
    win->bar_color       = spec->bar_color;
    win->content_color   = spec->content_color;
    win->visible         = spec->visible;
    win->visible_buttons = true;
    win->on_close        = spec->on_close;
    win->on_minimize     = spec->on_minimize;
    win->show_order      = win_count;

    wm_clamp(win);

    win_stack[win_count++] = win;
    wm_sort();
    return win;
}

void wm_unregister(window *win) {
    if (!win) return;

    for (int i = 0; i < win_count; i++) {
        if (win_stack[i] != win) continue;

        for (int j = i; j < win_count - 1; j++) win_stack[j] = win_stack[j + 1];

        win_stack[--win_count] = NULL;
        kfree(win);
        return;
    }
}

window *focused_window = NULL;

void wm_focus(window *win) {
    if (!win) return;

    focused_window = win;

    if (win->pinned_bottom) return;

    int old = win->show_order;
    for (int i = 0; i < win_count; i++)
        if (win_stack[i] != win && win_stack[i]->show_order > old
            && !win_stack[i]->pinned_bottom)
            win_stack[i]->show_order--;

    win->show_order = win_count - 1;
    wm_sort();
}

window *wm_focused_window(void) {
    if (focused_window && focused_window->visible && !focused_window->minimized)
        return focused_window;
    for (int i = win_count - 1; i >= 0; i--) {
        window *w = win_stack[i];
        if (w->visible && !w->minimized) return w;
    }
    return NULL;
}

void wm_sync_menubar(menubar *mb) {
    if (!mb) return;

    window *fw = wm_focused_window();

    const char *open_title = (mb->open_index >= 0) ? mb->menus[mb->open_index].title : NULL;

    mb->menu_count = 0;
    mb->open_index = -1;

    if (!fw || fw->win_menu_count == 0) return;

    for (int i = 0; i < fw->win_menu_count && mb->menu_count < MAX_MENUS; i++) {
        mb->menus[mb->menu_count] = fw->win_menus[i];
        mb->menus[mb->menu_count].open = false;

        if (open_title && strcmp(mb->menus[mb->menu_count].title, open_title) == 0) {
            mb->menus[mb->menu_count].open = true;
            mb->open_index = mb->menu_count;
        }
        mb->menu_count++;
    }
}

void wm_draw_all(void) {
    for (int i = 0; i < win_count; i++) {
        window *win = win_stack[i];
        if (win->visible && !win->minimized) window_draw(win);
    }

    int taskbar_x = 0;
    for (int i = 0; i < win_count; i++) {
        window *win = win_stack[i];
        if (!win->visible || !win->minimized) continue;

        int lw = taskbar_label_width(win);
        int ty = SCREEN_HEIGHT - TASKBAR_H;

        fill_rect(taskbar_x, ty, lw, TASKBAR_H, win->bar_color);
        draw_rect(taskbar_x, ty, lw, TASKBAR_H, BLACK);
        draw_string(taskbar_x + 4, ty + 2, (char *)win->title, win->title_color, 2);
        taskbar_x += lw + 1;
    }
}

void wm_update_all(void) {
    bool click_consumed = false;

    if (mouse.left_clicked && mouse.y >= SCREEN_HEIGHT - TASKBAR_H) {
        int tx = 0;
        for (int i = 0; i < win_count; i++) {
            window *win = win_stack[i];
            if (!win->visible || !win->minimized) continue;
            int lw = taskbar_label_width(win);
            if (mouse.x >= tx && mouse.x < tx + lw) {
                win->minimized = false;
                wm_focus(win);
                click_consumed = true;
                break;
            }
            tx += lw + 1;
        }
        if (click_consumed) return;
    }

    for (int i = win_count - 1; i >= 0; i--) {
        window *win = win_stack[i];
        if (!win->visible || win->minimized) continue;

        int wy = win->y + MENUBAR_H_SIZE;
        bool on_window = mouse.x >= win->x && mouse.x < win->x + win->w && mouse.y >= wy && mouse.y < wy + win->h;

        if (!click_consumed && mouse.left_clicked && on_window) {
            wm_focus(win);
            click_consumed = true;
            break;
        }
    }

    if (focused_window && focused_window->visible && !focused_window->minimized)
        window_update(focused_window);
}

void window_draw(window *win) {
    if (!win || !win->visible || win->minimized) return;

    int wy = win->y + MENUBAR_H_SIZE;

    if (win->dragging) {
        draw_rect(win->x + 2, wy + 2, win->w, win->h, BLACK);
        draw_rect(win->x, wy, win->w, win->h, LIGHT_GRAY);
        return;
    }

    if(win->visible_buttons) {
	    fill_rect(win->x, wy, win->w, 16, win->bar_color);

	    draw_titlebar_btn(win->x + 3,  wy + 3, 10, 10, "X", RED);
	    draw_titlebar_btn(win->x + 16, wy + 3, 10, 10, "_", LIGHT_BLUE);

	    int tx = win->x + win->w / 2 - (int)(strlen(win->title) * 2);
	    draw_string(tx, wy + 6, (char *)win->title, win->title_color, 2);

	    fill_rect(win->x, wy + 16, win->w, win->h - 16, win->content_color);
	    draw_rect(win->x, wy, win->w, win->h, BLACK);

		if (win->on_draw) win->on_draw(win, win->on_draw_userdata);
    }

    for (int i = 0; i < win->widget_count; i++)
        widget_draw(&win->widgets[i], win->x, wy);
}

bool window_update(window *win) {
    if (!win || !win->visible || win->minimized) return false;

    int wy = win->y + MENUBAR_H_SIZE;

    if (clicked_titlebar_btn(win->x + 3, wy + 3, 10, 10)) {
        if (win->on_close) win->on_close(win);
        else               win->visible = false;
        return false;
    }

    if (clicked_titlebar_btn(win->x + 16, wy + 3, 10, 10)) {
        if (win->on_minimize) win->on_minimize(win);
        else                  win->minimized = true;
        return true;
    }

    window_dragged(win);

    for (int i = 0; i < win->widget_count; i++)
        widget_update(&win->widgets[i], win->x, wy);

    return true;
}

void window_dragged(window *win) {
    if (!win || win->pinned_bottom) return;

    int wy = win->y + MENUBAR_H_SIZE;
    (void)wy;

    if (!win->dragging && mouse.left_clicked && in_titlebar(win, mouse.x, mouse.y)) win->dragging = true;

    if (win->dragging) {
        if (mouse.left) {
            win->x += mouse.dx;
            win->y += mouse.dy;
            wm_clamp(win);
        } else {
            win->dragging = false;
        }
    }
}

void window_add_widget(window *win, widget wg) {
    if (!win || win->widget_count >= MAX_WIN_WIDGETS) return;
    win->widgets[win->widget_count++] = wg;
}

menu *window_add_menu(window *win, const char *title) {
    if (!win || win->win_menu_count >= MAX_WIN_MENUS) return NULL;

    menu *m = &win->win_menus[win->win_menu_count++];
    memset(m, 0, sizeof(menu));
    m->title = (char *)title;
    m->open = false;
    m->item_count = 0;
    return m;
}
