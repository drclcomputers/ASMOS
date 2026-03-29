#include "ui/window.h"
#include "lib/utils.h"
#include "lib/string.h"
#include "io/mouse.h"

window *win_stack[MAX_WINDOWS];
int     win_count = 0;

static void wm_sort(void) {
    for (int i = 0; i < win_count - 1; i++)
        for (int j = i + 1; j < win_count; j++)
            if (win_stack[i]->show_order > win_stack[j]->show_order) {
                window *tmp  = win_stack[i];
                win_stack[i] = win_stack[j];
                win_stack[j] = tmp;
            }
}

static bool in_titlebar(window *win, int mx, int my) {
    return mx >= win->x && mx < win->x + win->w &&
           my >= win->y && my < win->y + 16;
}

static void draw_titlebar_btn(int ax, int ay, int w, int h,
                               char *label, unsigned char bg) {
    fill_rect(ax, ay, w, h, bg);
    draw_rect(ax, ay, w, h, 0x00);
    int tx = ax + w / 2 - (int)(strlen(label) * 3);
    int ty = ay + h / 2 - 3;
    draw_string(tx, ty, label, 0x00, 2);
}

static bool click_titlebar_btn(int ax, int ay, int w, int h) {
    return mouse.left_clicked &&
           mouse.x >= ax && mouse.x < ax + w &&
           mouse.y >= ay && mouse.y < ay + h;
}

void wm_register(window *win) {
    if (win_count >= MAX_WINDOWS) return;
    win->show_order   = win_count;
    win->widget_count = 0;
    win_stack[win_count++] = win;
    wm_sort();
}

void wm_unregister(window *win) {
    for (int i = 0; i < win_count; i++) {
        if (win_stack[i] == win) {
            for (int j = i; j < win_count - 1; j++)
                win_stack[j] = win_stack[j + 1];
            win_count--;
            return;
        }
    }
}

void wm_focus(window *win) {
    int old = win->show_order;
    for (int i = 0; i < win_count; i++)
        if (win_stack[i] != win && win_stack[i]->show_order > old)
            win_stack[i]->show_order--;
    win->show_order = win_count - 1;
    wm_sort();
}

void wm_draw_all(void) {
    int taskbar_x = 0;
    for (int i = 0; i < win_count; i++) {
        window *win = win_stack[i];
        if (!win->visible) continue;
        if (win->minimized) {
            int lw = strlen(win->title) * 5 + 8;
            fill_rect(taskbar_x, 200 - TASKBAR_H, lw, TASKBAR_H, win->bar_color);
            draw_rect(taskbar_x, 200 - TASKBAR_H, lw, TASKBAR_H, 0x00);
            draw_string(taskbar_x + 4, 200 - TASKBAR_H + 2,
                        win->title, win->title_color, 2);
            taskbar_x += lw + 1;
        } else {
            window_draw(win);
        }
    }
}

void wm_update_all(void) {
    bool click_consumed = false;
    for (int i = win_count - 1; i >= 0; i--) {
        window *win = win_stack[i];
        if (!win->visible) continue;
        if (win->minimized) {
            int taskbar_x = 0;
            for (int j = 0; j < i; j++)
                if (win_stack[j]->visible && win_stack[j]->minimized)
                    taskbar_x += strlen(win_stack[j]->title) * 5 + 9;
            int lw = strlen(win->title) * 5 + 8;
            bool on_label = mouse.x >= taskbar_x &&
                            mouse.x <  taskbar_x + lw &&
                            mouse.y >= 200 - TASKBAR_H;
            if (on_label && mouse.left_clicked && !click_consumed) {
                win->minimized = false;
                wm_focus(win);
                click_consumed = true;
            }
            continue;
        }
        if (!click_consumed && mouse.left_clicked &&
            in_titlebar(win, mouse.x, mouse.y)) {
            wm_focus(win);
            click_consumed = true;
        }
        if (i == win_count - 1)
            window_update(win);
    }
}

void window_draw(window *win) {
    if (!win->visible || win->minimized) return;
    fill_rect(win->x, win->y, win->w, 16, win->bar_color);
    draw_titlebar_btn(win->x + 3,  win->y + 3, 10, 10, "X", 0xCC);
    draw_titlebar_btn(win->x + 16, win->y + 3, 10, 10, "_", 0xA0);
    int tx = win->x + win->w / 2 - (int)(strlen(win->title) * 2);
    draw_string(tx, win->y + 6, win->title, win->title_color, 2);
    fill_rect(win->x, win->y + 16, win->w, win->h - 16, win->content_color);
    draw_rect(win->x, win->y, win->w, win->h, 0x00);
    for (int i = 0; i < win->widget_count; i++)
        widget_draw(&win->widgets[i], win->x, win->y);
}

bool window_update(window *win) {
    if (!win->visible || win->minimized) return false;
    if (click_titlebar_btn(win->x + 3, win->y + 3, 10, 10)) {
        if (win->on_close) win->on_close(NULL);
        else win->visible = false;
        return false;
    }
    if (click_titlebar_btn(win->x + 16, win->y + 3, 10, 10)) {
        if (win->on_minimize) win->on_minimize(NULL);
        else win->minimized = true;
        return true;
    }
    window_dragged(win);
    for (int i = 0; i < win->widget_count; i++)
        widget_update(&win->widgets[i], win->x, win->y);
    return true;
}

void window_add_widget(window *win, widget wg) {
    if (win->widget_count >= MAX_WIN_WIDGETS) return;
    win->widgets[win->widget_count++] = wg;
}

void window_dragged(window *win) {
    if (mouse.left && in_titlebar(win, mouse.x, mouse.y)) {
        win->x += mouse.dx;
        win->y += mouse.dy;

        if (win->x <= 0)
            win->x = 0;
        if (win->y <= 0)
            win->y = 0;
        if (win->x + win->w >= SCREEN_WIDTH)
            win->x = SCREEN_WIDTH - win->w;
        if (win->y + win->h >= SCREEN_HEIGHT)
            win->y = SCREEN_HEIGHT - win->h;
    }
}
