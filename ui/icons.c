#include "ui/icons.h"
#include "lib/primitive_graphics.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "io/mouse.h"
#include "interrupts/idt.h"
#include "config/config.h"
#include "ui/window.h"

static bool mouse_over_any_window(int mx, int my) {
    for (int i = 0; i < win_count; i++) {
        window *w = win_stack[i];
        if (!w->visible || w->minimized || w->pinned_bottom) continue;

        int wy = w->y + MENUBAR_H;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= wy    && my < wy  + w->h)
            return true;
    }
    return false;
}

static void grid_pos_local(const icon_view_t *v, int slot, int *out_x, int *out_y) {
    int rows_per_col = (v->area_h - ICON_START_Y) / ICON_SPACING_Y;
    if (rows_per_col < 1) rows_per_col = 1;

    int col = slot / rows_per_col;
    int row = slot % rows_per_col;

    *out_x = ICON_START_X + col * ICON_SPACING_X;
    *out_y = ICON_START_Y + row * ICON_SPACING_Y;
}

static int next_slot(const icon_view_t *v) {
    int slot = 0;
    for (int i = 0; i < MAX_ICONS_PER_VIEW; i++)
        if (v->icons[i].used) slot++;
    return slot;
}

static inline int abs_x(const icon_view_t *v, int lx) { return v->origin_x + lx; }
static inline int abs_y(const icon_view_t *v, int ly) { return v->origin_y + ly; }

static void make_display_label(const char *label, char *out) {
    int len = (int)strlen(label);
    if (len <= ICON_LABEL_DISPLAY_MAX) {
        strcpy(out, label);
    } else {
        int i;
        for (i = 0; i < ICON_LABEL_DISPLAY_MAX; i++)
            out[i] = label[i];
        out[i] = '\0';
    }
}

static void draw_default_icon(int ax, int ay, bool selected) {
    uint8_t fill = selected ? LIGHT_BLUE : WHITE;
    fill_rect(ax, ay, ICON_W, ICON_H, fill);
    draw_rect(ax, ay, ICON_W, ICON_H, BLACK);

    fill_rect(ax + 4, ay + 3, 8, 11, selected ? WHITE : LIGHT_GRAY);
    draw_rect(ax + 4, ay + 3, 8, 11, DARK_GRAY);

    draw_line(ax + 9, ay + 3, ax + 12, ay + 6, DARK_GRAY);
    draw_line(ax + 9, ay + 3, ax + 9,  ay + 6, DARK_GRAY);
    draw_line(ax + 9, ay + 6, ax + 12, ay + 6, DARK_GRAY);

    draw_line(ax + 6, ay + 8,  ax + 10, ay + 8,  DARK_GRAY);
    draw_line(ax + 6, ay + 10, ax + 10, ay + 10, DARK_GRAY);
}

void draw_file_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax+1, ay,      ICO_W-5, ICO_H,    bg);
    draw_rect(ax+1, ay,      ICO_W-5, ICO_H,    BLACK);
    fill_rect(ax+ICO_W-5, ay, 4, ICO_H,         BLACK);
    draw_line(ax+ICO_W-5, ay, ax+ICO_W-1, ay+4, BLACK);
    fill_rect(ax+ICO_W-4, ay, 3, 4,             bg);
    if (!sel) {
        draw_line(ax+3, ay+5,  ax+ICO_W-6, ay+5,  DARK_GRAY);
        draw_line(ax+3, ay+8,  ax+ICO_W-6, ay+8,  DARK_GRAY);
        draw_line(ax+3, ay+11, ax+ICO_W-6, ay+11, DARK_GRAY);
    }
}

void draw_folder_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax,     ay+3, ICO_W,   ICO_H-3, bg);
    draw_rect(ax,     ay+3, ICO_W,   ICO_H-3, BLACK);
    fill_rect(ax+1,   ay,   8,       4,        bg);
    draw_rect(ax+1,   ay,   8,       4,        BLACK);
    draw_line(ax,     ay+3, ax+1,    ay,       BLACK);
    draw_line(ax+9,   ay,   ax+9,    ay+3,     BLACK);
    if (sel)
        fill_rect(ax+2, ay+5, ICO_W-4, ICO_H-9, BLACK);
}

void draw_app_icon(int ax, int ay, bool sel) {
    uint8_t bg      = sel ? LIGHT_BLUE : WHITE;
    uint8_t title   = sel ? DARK_GRAY : BLUE;
    uint8_t content = sel ? WHITE : DARK_GRAY;

    fill_rect(ax+1, ay+1, ICO_W-2, ICO_H-2, bg);
    draw_rect(ax+1, ay+1, ICO_W-2, ICO_H-2, BLACK);

    fill_rect(ax+2, ay+2, ICO_W-4, 3, title);

    draw_line(ax+3, ay+7,  ax+ICO_W-4, ay+7,  content);
    draw_line(ax+3, ay+10, ax+ICO_W-4, ay+10, content);
    draw_line(ax+3, ay+13, ax+ICO_W-4, ay+13, content);
}

void draw_dotdot_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : LIGHT_GRAY;
    fill_rect(ax, ay+3, ICON_SZ_W, ICON_SZ_H-3, bg);
    draw_rect(ax, ay+3, ICON_SZ_W, ICON_SZ_H-3, BLACK);
    draw_string(ax+4, ay+8, "..", BLACK, 2);
}

void icon_view_init(icon_view_t *v, int origin_x, int origin_y, int area_w, int area_h) {
    memset(v, 0, sizeof(icon_view_t));
    v->origin_x        = origin_x;
    v->origin_y        = origin_y;
    v->area_w          = area_w - 15;
    v->area_h          = area_h - TASKBAR_H;
    v->_last_click_idx = -1;
    v->_drag_idx       = -1;
}

void icon_view_set_origin(icon_view_t *v, int origin_x, int origin_y, int area_w, int area_h) {
    v->origin_x = origin_x;
    v->origin_y = origin_y;
    v->area_w   = area_w;
    v->area_h   = area_h;
}

int icon_view_add(icon_view_t *v, const char *label, icon_action_t on_launch, int x, int y) {
    int idx = -1;
    for (int i = 0; i < MAX_ICONS_PER_VIEW; i++) {
        if (!v->icons[i].used) { idx = i; break; }
    }
    if (idx < 0) return -1;

    icon_t *ic = &v->icons[idx];
    memset(ic, 0, sizeof(icon_t));

    int li = 0;
    while (label[li] && li < ICON_LABEL_MAX - 1) {
        ic->label[li] = label[li];
        li++;
    }
    ic->label[li] = '\0';

    ic->on_launch = on_launch;
    ic->used      = true;

    if (x < 0 || y < 0) {
        grid_pos_local(v, next_slot(v) - 1, &ic->x, &ic->y);
    } else {
        ic->x = x;
        ic->y = y;
    }

    if (idx >= v->icon_count) v->icon_count = idx + 1;
    return idx;
}

void icon_view_remove(icon_view_t *v, int idx) {
    if (idx < 0 || idx >= MAX_ICONS_PER_VIEW) return;
    if (v->_drag_idx == idx) v->_drag_idx = -1;
    memset(&v->icons[idx], 0, sizeof(icon_t));
    while (v->icon_count > 0 && !v->icons[v->icon_count - 1].used)
        v->icon_count--;
}

icon_t *icon_view_get(icon_view_t *v, int *count_out) {
    if (count_out) *count_out = v->icon_count;
    return v->icons;
}

void icon_view_draw(const icon_view_t *v) {
    char disp[ICON_LABEL_DISPLAY_MAX + 4];

    for (int i = 0; i < MAX_ICONS_PER_VIEW; i++) {
        const icon_t *ic = &v->icons[i];
        if (!ic->used) continue;

        int ax = abs_x(v, ic->x);
        int ay = abs_y(v, ic->y);

        if (ic->dragging) {
            draw_rect(ax, ay, ICON_W, ICON_H, DARK_GRAY);
            continue;
        }

        if (ic->on_draw) {
            ic->on_draw(ax, ay);
        } else {
            draw_default_icon(ax, ay, ic->selected);
        }

        make_display_label(ic->label, disp);

        int label_w = (int)strlen(disp) * 5;
        int lx = ax + ICON_W / 2 - label_w / 2;
        int ly = ay + ICON_H + 2;

        draw_string(lx + 1, ly + 1, disp, BLACK, 2);
        draw_string(lx, ly, disp, WHITE, 2);
    }

    if (v->_drag_idx >= 0) {
        const icon_t *ic = &v->icons[v->_drag_idx];
        int ax = mouse.x - ic->drag_off_x;
        int ay = mouse.y - ic->drag_off_y;

        if (ic->on_draw) {
            ic->on_draw(ax, ay);
        } else {
            draw_default_icon(ax, ay, ic->selected);
        }

        make_display_label(ic->label, disp);
        int label_w = (int)strlen(disp) * 5;
        int lx = ax + ICON_W / 2 - label_w / 2;
        int ly = ay + ICON_H + 2;

        draw_string(lx + 1, ly + 1, disp, BLACK, 2);
        draw_string(lx, ly, disp, WHITE, 2);
    }
}

void icon_view_update(icon_view_t *v, bool blocked) {
    if (!blocked && mouse_over_any_window(mouse.x, mouse.y)) {
        if (v->_drag_idx >= 0 && !mouse.left) {
            v->icons[v->_drag_idx].dragging = false;
            v->_drag_idx = -1;
        }
        return;
    }

    if (blocked) {
        if (v->_drag_idx >= 0 && !mouse.left) {
            v->icons[v->_drag_idx].dragging = false;
            v->_drag_idx = -1;
        }
        return;
    }

    if (v->_drag_idx >= 0) {
        icon_t *ic = &v->icons[v->_drag_idx];

        if (mouse.left) {
            int new_lx = mouse.x - ic->drag_off_x - v->origin_x;
            int new_ly = mouse.y - ic->drag_off_y - v->origin_y;

            if (new_lx < 0) new_lx = 0;
            if (new_ly < 0) new_ly = 0;
            if (new_lx + ICON_W > v->area_w) new_lx = v->area_w - ICON_W;
            if (new_ly + ICON_H > v->area_h) new_ly = v->area_h - ICON_H;

            ic->x = new_lx;
            ic->y = new_ly;
        } else {
            ic->dragging = false;
            v->_drag_idx = -1;
        }
        return;
    }

    for (int i = 0; i < MAX_ICONS_PER_VIEW; i++) {
        icon_t *ic = &v->icons[i];
        if (!ic->used) continue;

        int ax = abs_x(v, ic->x);
        int ay = abs_y(v, ic->y);

        bool hit = mouse.x >= ax && mouse.x < ax + ICON_W
                && mouse.y >= ay && mouse.y < ay + ICON_H;

        if (!hit) continue;

        if (mouse.left_clicked) {
            for (int j = 0; j < MAX_ICONS_PER_VIEW; j++)
                v->icons[j].selected = false;
            ic->selected = true;

            uint32_t now = pit_ticks;
            if (v->_last_click_idx == i &&
                (now - v->_last_click_tick) <= DBLCLICK_TICKS) {
                if (ic->on_launch) ic->on_launch();
                v->_last_click_idx  = -1;
                v->_last_click_tick = 0;
            } else {
                v->_last_click_idx  = i;
                v->_last_click_tick = now;
            }
            return;
        }

        if (mouse.left && !mouse.left_clicked) {
            int moved = mouse.dx * mouse.dx + mouse.dy * mouse.dy;
            if (moved >= DRAG_THRESHOLD * DRAG_THRESHOLD ||
                (mouse.dx != 0 || mouse.dy != 0)) {
                ic->dragging    = true;
                ic->drag_off_x  = mouse.x - ax;
                ic->drag_off_y  = mouse.y - ay;
                v->_drag_idx    = i;

                v->_last_click_idx  = -1;
                v->_last_click_tick = 0;
            }
            return;
        }
    }

    if (mouse.left_clicked) {
        bool inside_view = mouse.x >= v->origin_x
                        && mouse.x <  v->origin_x + v->area_w
                        && mouse.y >= v->origin_y
                        && mouse.y <  v->origin_y + v->area_h;
        if (inside_view) {
            for (int i = 0; i < MAX_ICONS_PER_VIEW; i++)
                v->icons[i].selected = false;
        }
    }
}

static icon_view_t s_desktop_view;

void desktop_icons_init(int origin_x, int origin_y, int area_w, int area_h) {
    icon_view_init(&s_desktop_view, origin_x, origin_y, area_w, area_h);
}

int desktop_icon_add(const char *label, icon_action_t on_launch, int x, int y) {
    return icon_view_add(&s_desktop_view, label, on_launch, x, y);
}

void desktop_icon_remove(int idx) {
    icon_view_remove(&s_desktop_view, idx);
}

icon_t *desktop_icons_get(int *count_out) {
    return icon_view_get(&s_desktop_view, count_out);
}

void desktop_icons_update(bool window_captured) {
    if (window_captured) return;
    icon_view_update(&s_desktop_view, false);
}

void desktop_icons_draw(void) {
    icon_view_draw(&s_desktop_view);
}
