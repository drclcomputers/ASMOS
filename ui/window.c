#include "ui/window.h"
#include "ui/menubar.h"

#include "os/error.h"
#include "os/os.h"

#include "lib/memory.h"
#include "lib/string.h"
#include "lib/graphics.h"

#include "io/mouse.h"
#include "config/config.h"

window *win_stack[MAX_WINDOWS];
int     win_count = 0;
window *focused_window = NULL;

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
    if (win->x + win->w > SCREEN_WIDTH)  win->x = SCREEN_WIDTH  - win->w;
    if (win->y + win->h + MENUBAR_H_SIZE + TASKBAR_H > SCREEN_HEIGHT)
        win->y = SCREEN_HEIGHT - win->h - MENUBAR_H_SIZE - TASKBAR_H;
}

static bool in_titlebar(const window *win, int mx, int my) {
    int wy = win->y + MENUBAR_H_SIZE;
    return mx >= win->x && mx < win->x + win->w && my >= wy && my < wy + TITLEBAR_H;
}

static void draw_titlebar_btn(int ax, int ay, int w, int h, const char *label, uint8_t bg) {
    fill_rect(ax, ay, w, h, bg);
    draw_rect(ax, ay, w, h, BLACK);
    int tx = ax + w / 2 - (int)(strlen(label) * 3) + 1;
    int ty = ay + h / 2 - 3 + 1;
    draw_string(tx, ty, (char *)label, WHITE, 2);
}

static bool clicked_titlebar_btn(int ax, int ay, int w, int h) {
    if (!mouse.left_clicked)       return false;
    if (g_menubar_click_consumed)  return false;
    return mouse.x >= ax && mouse.x < ax + w
        && mouse.y >= ay && mouse.y < ay + h;
}

static int taskbar_label_width(const window *win) {
    return (int)strlen(win->title) * 5 + 8;
}

window *wm_register(const window_spec_t *spec) {
    if (!spec) {
        ERR_WARN_REPORT(ERR_NULL_PTR, "wm_register: spec");
        return NULL;
    }
    if (win_count >= MAX_WINDOWS) {
        ERR_WARN_REPORT(ERR_WM_MAX_WINDOWS, "wm_register");
        return NULL;
    }
    if (spec->w <= 0 || spec->h <= 0) {
        ERR_WARN_REPORT(ERR_INVALID_ARG, "wm_register: w/h <= 0");
        return NULL;
    }

    window *win = (window *)kmalloc(sizeof(window));
    if (!win) {
        ERR_WARN_REPORT(ERR_WM_ALLOC, "wm_register: kmalloc");
        return NULL;
    }

    memset(win, 0, sizeof(window));

    win->x             = spec->x;
    win->y             = spec->y;
    win->w             = spec->w;
    win->h             = spec->h;
    win->min_w         = spec->min_w;
    win->min_h         = spec->min_h;
    win->max_w         = spec->max_w;
    win->max_h         = spec->max_h;
    win->resizable     = spec->resizable;
    win->title         = spec->title ? spec->title : "";
    win->title_color   = spec->title_color;
    win->bar_color     = spec->bar_color;
    win->content_color = spec->content_color;
    win->visible       = spec->visible;
    win->visible_buttons = true;
    win->on_close      = spec->on_close;
    win->on_minimize   = spec->on_minimize;
    win->show_order    = win_count;

    wm_clamp(win);

    win_stack[win_count++] = win;
    wm_sort();

    if (spec->visible && !win->pinned_bottom) {
        wm_focus(win);
    }

    return win;
}

void wm_unregister(window *win) {
    if (!win) return;

    for (int i = 0; i < win_count; i++) {
        if (win_stack[i] != win) continue;

        if (focused_window == win) focused_window = NULL;

        for (int j = i; j < win_count - 1; j++) win_stack[j] = win_stack[j + 1];
        win_stack[--win_count] = NULL;
        kfree(win);

        for (int k = win_count - 1; k >= 0; k--) {
            if (win_stack[k]->visible && !win_stack[k]->minimized
                    && !win_stack[k]->pinned_bottom) {
                wm_focus(win_stack[k]);
                break;
            }
        }
        return;
    }
}

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

    const char *open_title = (mb->open_index >= 0)
                             ? mb->menus[mb->open_index].title : NULL;
    mb->menu_count = 0;
    mb->open_index = -1;

    if (!fw || fw->win_menu_count == 0) return;

    for (int i = 0; i < fw->win_menu_count && mb->menu_count < MAX_MENUS; i++) {
        mb->menus[mb->menu_count] = fw->win_menus[i];
        mb->menus[mb->menu_count].open = false;
        if (open_title &&
            strcmp(mb->menus[mb->menu_count].title, open_title) == 0) {
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

    if (mouse.left_clicked && mouse.y < MENUBAR_H_SIZE) {
        return;
    }

    if (!g_menubar_click_consumed) {
        for (int i = win_count - 1; i >= 0; i--) {
            window *win = win_stack[i];
            if (!win->visible || win->minimized) continue;
            int wy = win->y + MENUBAR_H_SIZE;
            bool on_win = mouse.x >= win->x && mouse.x <= win->x + win->w
                            && mouse.y >= wy     && mouse.y <= wy + win->h;
            if (!click_consumed && mouse.left_clicked && on_win) {
                wm_focus(win);
                click_consumed = true;
                break;
            }
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
        draw_rect(win->x,     wy,     win->w, win->h, LIGHT_GRAY);
        return;
    }

        if (win->visible_buttons) {
        fill_rect(win->x, wy, win->w, 16, win->bar_color);
        draw_titlebar_btn(win->x + 3,  wy + 3, 10, 10, "X", RED);
        draw_titlebar_btn(win->x + 16, wy + 3, 10, 10, "_", LIGHT_BLUE);

        const char *title = win->title;
        char title_buf[256];

        int char_pixel_w = 6;

        int max_width = win->w - 30;
        int max_chars = max_width / char_pixel_w;

        if (max_chars > 3 && (int)strlen(title) > max_chars) {
            int keep_chars = max_chars - 3;
            strncpy(title_buf, title, keep_chars);
            title_buf[keep_chars] = '\0';
            strcat(title_buf, "...");
            title = title_buf;
        }

        int button_offset = 28;
        int title_area_center = (button_offset + win->w) / 2;
        int tx = win->x + title_area_center - (int)(strlen(title) * (char_pixel_w / 2));

        if (tx < win->x + button_offset + 5) {
            tx = win->x + button_offset + 5;
        }

        draw_string(tx, wy + 6, (char *)title, win->title_color, 2);

        fill_rect(win->x, wy + 16, win->w, win->h - 16, win->content_color);
        draw_rect(win->x, wy,      win->w, win->h,      BLACK);

        if (win->on_draw) win->on_draw(win, win->on_draw_userdata);

        if (win->resizable) {
            int gx = win->x + win->w - 10;
            int gy = win->y + MENUBAR_H_SIZE + win->h - 10;
            fill_rect(gx, gy, 10, 10, LIGHT_GRAY);
            draw_rect(gx, gy, 10, 10, DARK_GRAY);
            draw_line(gx + 3, gy + 8, gx + 8, gy + 3, DARK_GRAY);
            draw_line(gx + 6, gy + 8, gx + 8, gy + 6, DARK_GRAY);
        }
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

    window_resize(win);
    window_dragged(win);

    for (int i = 0; i < win->widget_count; i++)
        widget_update(&win->widgets[i], win->x, wy);

    return true;
}

void window_resize(window *win) {
    if (!win || !win->resizable) return;

    int gx = win->x + win->w - 10;
    int gy = win->y + MENUBAR_H_SIZE + win->h - 10;

    if (!win->resizing && mouse.left_clicked
        && mouse.x >= gx && mouse.x < gx + 10
        && mouse.y >= gy && mouse.y < gy + 10)
        win->resizing = true;

    if (win->resizing) {
        if (mouse.left) {
            win->w += mouse.dx;
            win->h += mouse.dy;
            int min_w = win->min_w > 0 ? win->min_w : 60;
            int min_h = win->min_h > 0 ? win->min_h : 40;
            if (win->w < min_w) win->w = min_w;
            if (win->h < min_h) win->h = min_h;
            int max_w = SCREEN_WIDTH  - win->x;
            int max_h = SCREEN_HEIGHT - MENUBAR_H_SIZE - TASKBAR_H - win->y;
            if (win->w > max_w) win->w = max_w;
            if (win->h > max_h) win->h = max_h;
            if (win->max_w > 0 && win->w > win->max_w) win->w = win->max_w;
            if (win->max_h > 0 && win->h > win->max_h) win->h = win->max_h;
        } else {
            win->resizing = false;
        }
    }
}

void window_dragged(window *win) {
    if (!win || win->pinned_bottom || win->resizing) return;

    if (!win->dragging && mouse.left_clicked && in_titlebar(win, mouse.x, mouse.y))
        win->dragging = true;

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
    if (!win) {
        ERR_WARN_REPORT(ERR_NULL_PTR, "window_add_widget: win");
        return;
    }
    if (win->widget_count >= MAX_WIN_WIDGETS) {
        ERR_WARN_REPORT(ERR_INVALID_ARG, "window_add_widget: max widgets");
        return;
    }
    win->widgets[win->widget_count++] = wg;
}

menu *window_add_menu(window *win, const char *title) {
    if (!win) {
        ERR_WARN_REPORT(ERR_NULL_PTR, "window_add_menu: win");
        return NULL;
    }
    if (win->win_menu_count >= MAX_WIN_MENUS) {
        ERR_WARN_REPORT(ERR_INVALID_ARG, "window_add_menu: max menus");
        return NULL;
    }
    menu *m = &win->win_menus[win->win_menu_count++];
    memset(m, 0, sizeof(menu));
    m->title      = (char *)title;
    m->open       = false;
    m->item_count = 0;
    return m;
}
