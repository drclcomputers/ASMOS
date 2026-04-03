#include "ui/widgets.h"
#include "lib/primitive_graphics.h"
#include "lib/string.h"
#include "io/mouse.h"
#include "io/keyboard.h"

static inline int abs_x(int win_x, int wx) { return win_x + 1 + wx; }
static inline int abs_y(int win_y, int wy) { return win_y + 17 + wy; }

static inline bool mouse_over(int ax, int ay, int w, int h) {
    return mouse.x >= ax && mouse.x < ax + w &&
           mouse.y >= ay && mouse.y < ay + h;
}

static void draw_button(widget *wg, int ax, int ay) {
    fill_rect(ax, ay, wg->w, wg->h, wg->bg_color);
    draw_rect(ax, ay, wg->w, wg->h, wg->border_color);
    int tx = ax + wg->w / 2 - (int)(strlen(wg->as.button.label) * 3);
    int ty = ay + wg->h / 2 - 3;
    draw_string(tx, ty, wg->as.button.label, wg->fg_color, 2);
}

static void draw_label(widget *wg, int ax, int ay) {
    draw_string(ax, ay, wg->as.label.text, wg->as.label.color, wg->as.label.font_size);
}

static void draw_checkbox(widget *wg, int ax, int ay) {
    widget_checkbox *cb = &wg->as.checkbox;
    fill_rect(ax, ay, 10, 10, wg->bg_color);
    draw_rect(ax, ay, 10, 10, wg->border_color);
    if (cb->checked) draw_string(ax + 2, ay + 2, "x", wg->fg_color, 2);
    draw_string(ax + 14, ay + 2, cb->label, wg->fg_color, 2);
}

static void draw_textbox(widget *wg, int ax, int ay) {
    widget_textbox *tb = &wg->as.textbox;
    unsigned char border = tb->focused ? BLUE : wg->border_color;
    fill_rect(ax, ay, wg->w, wg->h, wg->bg_color);
    draw_rect(ax, ay, wg->w, wg->h, border);
    draw_string(ax + 2, ay + 2, tb->buf, wg->fg_color, 2);
    if (tb->focused) {
        int cx = ax + 2 + tb->len * 5;
        if (cx < ax + wg->w - 4) draw_string(cx, ay + 2, "|", wg->fg_color, 2);
    }
}

static void draw_dropdown(widget *wg, int ax, int ay) {
    widget_dropdown *dd = &wg->as.dropdown;
    fill_rect(ax, ay, wg->w, wg->h, wg->bg_color);
    draw_rect(ax, ay, wg->w, wg->h, wg->border_color);
    if (dd->selected >= 0 && dd->selected < dd->item_count)
    	draw_string(ax + 2, ay + 2, dd->items[dd->selected], wg->fg_color, 2);
    draw_string(ax + wg->w - 8, ay + 2, dd->open ? "^" : "v", wg->fg_color, 2);
    if (dd->open) {
        for (int i = 0; i < dd->item_count; i++) {
            int iy = ay + wg->h * (i + 1);
            unsigned char bg = (i == dd->selected) ? LIGHT_GRAY : wg->bg_color;
            fill_rect(ax, iy, wg->w, wg->h, bg);
            draw_rect(ax, iy, wg->w, wg->h, wg->border_color);
            draw_string(ax + 2, iy + 2, dd->items[i], wg->fg_color, 2);
        }
    }
}

static void draw_menu(widget *wg, int ax, int ay) {
    widget_menu *m = &wg->as.menu;
    fill_rect(ax, ay, wg->w, wg->h, wg->bg_color);
    draw_rect(ax, ay, wg->w, wg->h, wg->border_color);
    draw_string(ax + 2, ay + 2, "Menu", wg->fg_color, 2);
    if (m->open) {
        for (int i = 0; i < m->item_count; i++) {
            int iy = ay + wg->h * (i + 1);
            fill_rect(ax, iy, wg->w, wg->h, wg->bg_color);
            draw_rect(ax, iy, wg->w, wg->h, wg->border_color);
            draw_string(ax + 4, iy + 2, m->items[i], wg->fg_color, 2);
        }
    }
}

void widget_draw(widget *wg, int win_x, int win_y) {
    int ax = abs_x(win_x, wg->x);
    int ay = abs_y(win_y, wg->y);
    switch (wg->type) {
        case WIDGET_BUTTON:   draw_button(wg, ax, ay);   break;
        case WIDGET_LABEL:    draw_label(wg, ax, ay);    break;
        case WIDGET_CHECKBOX: draw_checkbox(wg, ax, ay); break;
        case WIDGET_TEXTBOX:  draw_textbox(wg, ax, ay);  break;
        case WIDGET_DROPDOWN: draw_dropdown(wg, ax, ay); break;
        case WIDGET_MENU:     draw_menu(wg, ax, ay);     break;
    }
}

static void update_button(widget *wg, int ax, int ay) {
    if (mouse_over(ax, ay, wg->w, wg->h) && mouse.left_clicked)
        if (wg->as.button.on_click) wg->as.button.on_click(wg);
}

static void update_checkbox(widget *wg, int ax, int ay) {
    widget_checkbox *cb = &wg->as.checkbox;
    if (mouse_over(ax, ay, 10, 10) && mouse.left_clicked) {
        cb->checked = !cb->checked;
        if (cb->on_change) cb->on_change(wg);
    }
}

static void update_textbox(widget *wg, int ax, int ay) {
    widget_textbox *tb = &wg->as.textbox;
    if (mouse.left_clicked) {
        if (mouse_over(ax, ay, wg->w, wg->h)) tb->focused = true;
        else if (tb->focused) tb->focused = false;
    }
    if (!tb->focused) return;
    if (kb.key_pressed && kb.last_char && kb.last_char != '\b') {
        if (tb->len < (int)sizeof(tb->buf) - 1) {
            tb->buf[tb->len++] = kb.last_char;
            tb->buf[tb->len]   = '\0';
        }
    }
    if (kb.key_pressed && kb.last_scancode == BACKSPACE && tb->len > 0)
        tb->buf[--tb->len] = '\0';
}

static void update_dropdown(widget *wg, int ax, int ay) {
    widget_dropdown *dd = &wg->as.dropdown;
    if (!mouse.left_clicked) return;
    if (mouse_over(ax, ay, wg->w, wg->h)) { dd->open = !dd->open; return; }
    if (dd->open) {
        for (int i = 0; i < dd->item_count; i++) {
            int iy = ay + wg->h * (i + 1);
            if (mouse_over(ax, iy, wg->w, wg->h)) {
                dd->selected = i;
                dd->open = false;
                if (dd->on_select) dd->on_select(wg);
                return;
            }
        }
        dd->open = false;
    }
}

static void update_menu(widget *wg, int ax, int ay) {
    widget_menu *m = &wg->as.menu;
    if (!mouse.left_clicked) return;
    if (mouse_over(ax, ay, wg->w, wg->h)) { m->open = !m->open; return; }
    if (m->open) {
        for (int i = 0; i < m->item_count; i++) {
            int iy = ay + wg->h * (i + 1);
            if (mouse_over(ax, iy, wg->w, wg->h)) {
                if (m->callbacks[i]) m->callbacks[i](wg);
                m->open = false;
                return;
            }
        }
        m->open = false;
    }
}

void widget_update(widget *wg, int win_x, int win_y) {
    int ax = abs_x(win_x, wg->x);
    int ay = abs_y(win_y, wg->y);
    switch (wg->type) {
        case WIDGET_BUTTON:   update_button(wg, ax, ay);   break;
        case WIDGET_LABEL:                                  break;
        case WIDGET_CHECKBOX: update_checkbox(wg, ax, ay); break;
        case WIDGET_TEXTBOX:  update_textbox(wg, ax, ay);  break;
        case WIDGET_DROPDOWN: update_dropdown(wg, ax, ay); break;
        case WIDGET_MENU:     update_menu(wg, ax, ay);     break;
    }
}
