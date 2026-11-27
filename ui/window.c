// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "ui/window.h"
#include "ui/dialogs.h"
#include "ui/menubar.h"
#include "ui/uifont.h"

#include "os/error.h"
#include "os/os.h"

#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"

#include "config/config.h"
#include "config/runtime_config.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "lib/time.h"
#include "os/prefs.h"
#include "os/watchdog.h"
#include "ui/cursor.h"
#include "ui/desktop.h"
#include "ui/popup.h"

window *win_stack[MAX_WINDOWS];
int win_count = 0;
window *focused_window = NULL;

static void wm_sort(void) {
    for (int i = 0; i < win_count - 1; i++) {
        for (int j = i + 1; j < win_count; j++) {
            if (win_stack[i]->show_order > win_stack[j]->show_order) {
                window *tmp = win_stack[i];
                win_stack[i] = win_stack[j];
                win_stack[j] = tmp;
            }
        }
    }
}

/* on the screen: the right/bottom edge first, then the left/top, so a
 * window bigger than the screen starts at 0, 0 (fit_to_screen() shrinks
 * it before it is drawn) */
static void clamp_xy(int *x, int *y, int w, int h) {
    if (*x + w > SCREEN_WIDTH)
        *x = SCREEN_WIDTH - w;
    if (*y + h + MENUBAR_H + TASKBAR_H > SCREEN_HEIGHT)
        *y = SCREEN_HEIGHT - h - MENUBAR_H - TASKBAR_H;
    if (*x < 0)
        *x = 0;
    if (*y < 0)
        *y = 0;
}

static void wm_clamp(window *win) {
    clamp_xy(&win->x, &win->y, win->w, win->h);
}

static bool in_titlebar(const window *win, int mx, int my) {
    int wy = win->y + MENUBAR_H;
    return mx >= win->x && mx < win->x + win->w && my >= wy &&
           my < wy + TITLEBAR_H;
}

static void draw_titlebar_btn(int ax, int ay, int w, int h, const char *label,
                              uint8_t bg) {
    fill_rect(ax, ay, w, h, bg);
    draw_rect(ax, ay, w, h, BLACK);
    /* drawn, not a font glyph: centred exactly, 2 px strokes to match the
     * bold title (a 6x6 X; a 1 px underscore low in the box) */
    int s6 = 6, x0 = ax + (w - s6) / 2, y0 = ay + (h - s6) / 2;
    if (label[0] == 'X') {
        for (int i = 0; i < s6; i++) {
            fill_rect(x0 + i, y0 + i, i < s6 - 1 ? 2 : 1, 1, WHITE);
            fill_rect(x0 + s6 - 1 - i - (i < s6 - 1 ? 1 : 0), y0 + i,
                      i < s6 - 1 ? 2 : 1, 1, WHITE);
        }
    } else {
        fill_rect(x0, y0 + s6 - 1, s6, 1, WHITE);
    }
}

static bool clicked_titlebar_btn(int ax, int ay, int w, int h) {
    if (!mouse.left_clicked)
        return false;
    if (g_menubar_click_consumed)
        return false;
    return mouse.x >= ax && mouse.x < ax + w && mouse.y >= ay &&
           mouse.y < ay + h;
}

static int taskbar_label_width(const window *win) {
    return text_width(win->title, UI_FONT_BOLD) + 8;
}

static int taskbar_x_for(const window *target) {
    int tx = 0;
    for (int i = 0; i < win_count; i++) {
        window *w = win_stack[i];
        if (w == target)
            break;
        if (w->visible && w->minimized)
            tx += taskbar_label_width(w) + 1;
    }
    return tx;
}

bool is_desktop_focused() {
    window *fw = wm_focused_window();

    if (fw != NULL && fw->pinned_bottom) {
        return true;
    }

    return false;
}

static void draw_anim(window *win) {
    int wy = win->y + MENUBAR_H;

    int win_rx = win->x, win_ry = wy, win_rw = win->w, win_rh = win->h;

    int tb_x = taskbar_x_for(win);
    int tb_y = SCREEN_HEIGHT - TASKBAR_H;
    int tb_w = taskbar_label_width(win);
    int tb_h = TASKBAR_H;

    int cx = win->x + win->w / 2;
    int cy = wy + win->h / 2;

    int f = win->anim_frame;
    int t = win->anim_no_of_frames;

    int rx, ry, rw, rh;

    switch (win->anim_state) {
    case WIN_ANIM_OPEN:
        rw = lerp_i(2, win_rw, f, t);
        rh = lerp_i(2, win_rh, f, t);
        rx = cx - rw / 2;
        ry = cy - rh / 2;
        break;
    case WIN_ANIM_CLOSE:
        rw = lerp_i(win_rw, 2, f, t);
        rh = lerp_i(win_rh, 2, f, t);
        rx = cx - rw / 2;
        ry = cy - rh / 2;
        break;
    case WIN_ANIM_MINIMIZE:
        rx = lerp_i(win_rx, tb_x, f, t);
        ry = lerp_i(win_ry, tb_y, f, t);
        rw = lerp_i(win_rw, tb_w, f, t);
        rh = lerp_i(win_rh, tb_h, f, t);
        break;
    case WIN_ANIM_RESTORE:
        rx = lerp_i(tb_x, win_rx, f, t);
        ry = lerp_i(tb_y, win_ry, f, t);
        rw = lerp_i(tb_w, win_rw, f, t);
        rh = lerp_i(tb_h, win_rh, f, t);
        break;
    default:
        return;
    }

    if (rw < 2)
        rw = 2;
    if (rh < 2)
        rh = 2;

    draw_rect(rx + 2, ry + 2, rw, rh, BLACK);
    draw_rect(rx, ry, rw, rh, WHITE);
    draw_rect(rx + 1, ry + 1, rw - 2, rh - 2, win->bar_color);
}

void wm_init(void) {
    win_count = 0;
    focused_window = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++)
        win_stack[i] = NULL;
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
        return NULL;
    }

    memset(win, 0, sizeof(window));

    win->x = spec->x;
    win->y = spec->y;
    win->w = spec->w;
    win->h = spec->h;
    win->min_w = spec->min_w;
    win->min_h = spec->min_h;
    win->max_w = spec->max_w;
    win->max_h = spec->max_h;
    win->resizable = spec->resizable;
    win->title = spec->title ? spec->title : "";
    win->title_color = spec->title_color;
    win->bar_color = spec->bar_color;
    win->content_color = spec->content_color;
    win->visible = spec->visible;
    win->visible_buttons = true;
    win->on_close = spec->on_close;
    win->on_minimize = spec->on_minimize;
    win->show_order = win_count;
    win->animate_open_close = true;
    win->anim_no_of_frames = WIN_ANIM_DEFAULT_FRAMES;

    wm_clamp(win);

    win->pinned_bottom = spec->pinned_bottom;

    if (spec->visible && !spec->pinned_bottom && win->animate_open_close &&
        !g_cfg.reduce_motion) {
        win->anim_state = WIN_ANIM_OPEN;
        win->anim_frame = 0;
    } else {
        win->anim_state = WIN_ANIM_NONE;
    }

    win_stack[win_count++] = win;
    wm_sort();

    if (spec->visible && !win->pinned_bottom)
        wm_focus(win);

    return win;
}

// remembered positions: /PREFS/WINDOWS.INI, "<app name>=x,y"

#define POS_FILE "WINDOWS"
static const char *s_launch_key;

void wm_set_launch_key(const char *name) { s_launch_key = name; }

const char *wm_take_launch_key(void) {
    const char *k = s_launch_key;
    s_launch_key = NULL; /* only the first window */
    return k;
}

static void pos_key_of(const char *name, char *out) {
    int k = 0;
    for (const char *p = name; *p && k < 23; p++)
        if (*p != '=' && *p != '\n' && *p != '\r')
            out[k++] = *p;
    out[k] = 0;
}

static int parse_int(const char **p) {
    int sign = 1, v = 0;
    if (**p == '-') {
        sign = -1;
        (*p)++;
    }
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (*(*p)++ - '0');
    return sign * v;
}

bool wm_saved_pos(const char *name, int *x, int *y) {
    char key[24], v[32];
    bool found = false;
    pos_key_of(name, key);
    if (key[0] && !g_setup_mode && app_prefs_get(POS_FILE, key, v, sizeof(v))) {
        const char *p = v;
        int sx = parse_int(&p);
        if (*p == ',') {
            p++;
            *x = sx;
            *y = parse_int(&p);
            found = true;
        }
    }
    /* another window of the same app already there: a little lower right,
     * so the new one doesn't hide it */
    for (int n = 0; n < 8; n++) {
        bool taken = false;
        for (int i = 0; i < win_count; i++)
            if (!strcmp(win_stack[i]->pos_key, key) && win_stack[i]->x == *x &&
                win_stack[i]->y == *y)
                taken = true;
        if (!taken)
            break;
        *x += 12;
        *y += 12;
    }
    return found;
}

/* where a remembered window is now (zoomed: where it goes back to) */
static void pos_now(const window *w, int *x, int *y) {
    *x = w->zoomed ? w->zx : w->x;
    *y = w->zoomed ? w->zy : w->y;
}

static bool pos_moved(const window *w) {
    int x, y;
    pos_now(w, &x, &y);
    return w->pos_key[0] && (x != w->pos_x0 || y != w->pos_y0);
}

static void pos_put(window *w) {
    char v[24];
    int x, y;
    pos_now(w, &x, &y);
    snprintf(v, sizeof(v), "%d,%d", x, y);
    app_prefs_set(POS_FILE, w->pos_key, v);
    w->pos_x0 = x;
    w->pos_y0 = y;
}

void wm_save_positions(void) {
    if (g_setup_mode)
        return;
    bool any = false;
    for (int i = 0; i < win_count; i++)
        if (pos_moved(win_stack[i])) {
            if (!any)
                app_prefs_begin(POS_FILE); /* one write for all of them */
            any = true;
            pos_put(win_stack[i]);
        }
    if (any)
        app_prefs_end();
}

void wm_unregister(window *win) {
    if (!win || !wm_is_registered(win))
        return;
    if (pos_moved(win) && !g_setup_mode) /* next time it opens here */
        pos_put(win);
    dlg_owner_destroyed(win);
    if (win->modal_owner && win->modal_owner->modal_child == win)
        win->modal_owner->modal_child = NULL;
    ctl_window_destroyed(win);

    for (int i = 0; i < win_count; i++) {
        if (win_stack[i] != win)
            continue;

        if (focused_window == win)
            focused_window = NULL;

        for (int j = i; j < win_count - 1; j++)
            win_stack[j] = win_stack[j + 1];
        win_stack[--win_count] = NULL;
        for (int m = 0; m < win->win_menu_count; m++)
            menu_free_subs(&win->win_menus[m]);
        kfree(win);

        for (int k = win_count - 1; k >= 0; k--) {
            if (win_stack[k]->visible && !win_stack[k]->minimized &&
                !win_stack[k]->pinned_bottom) {
                wm_focus(win_stack[k]);
                break;
            }
        }
        return;
    }
}

void wm_focus(window *win) {
    if (!win)
        return;

    focused_window = win;
    if (win->pinned_bottom)
        return;

    int old = win->show_order;
    for (int i = 0; i < win_count; i++)
        if (win_stack[i] != win && win_stack[i]->show_order > old &&
            !win_stack[i]->pinned_bottom)
            win_stack[i]->show_order--;

    win->show_order = win_count - 1;
    wm_sort();
}

window *wm_focused_window(void) {
    if (focused_window && focused_window->visible && !focused_window->minimized)
        return focused_window;
    for (int i = win_count - 1; i >= 0; i--) {
        window *w = win_stack[i];
        if (w->visible && !w->minimized)
            return w;
    }
    return NULL;
}

void wm_sync_menubar(menubar *mb) {
    if (!mb)
        return;

    window *fw = wm_focused_window();

    const char *open_title =
        (mb->open_index >= 0) ? mb->menus[mb->open_index].title : NULL;
    int sub = mb->sub_item;
    mb->menu_count = 0;
    mb->open_index = -1;
    mb->sub_item = -1;
    if (mb->owner != fw) { /* another window: its first page */
        mb->owner = fw;
        mb->first = mb->npages = 0;
    }

    /* the desktop's Apps menu comes first on every app's bar: no need to
     * go back to the desktop to start another app */
    window *desk = NULL;
    for (int i = 0; i < win_count && !desk; i++)
        if (win_stack[i]->pinned_bottom)
            desk = win_stack[i];
    const menu *apps = NULL;
    for (int i = 0; desk && desk != fw && i < desk->win_menu_count; i++)
        if (desk->win_menus[i].title &&
            !strcmp(desk->win_menus[i].title, "Apps"))
            apps = &desk->win_menus[i];

    int n = fw ? fw->win_menu_count : 0;
    for (int i = apps ? -1 : 0; i < n && mb->menu_count < MAX_MENUS; i++) {
        mb->menus[mb->menu_count] = i < 0 ? *apps : fw->win_menus[i];
        mb->menus[mb->menu_count].open = false;
        if (open_title &&
            strcmp(mb->menus[mb->menu_count].title, open_title) == 0) {
            mb->menus[mb->menu_count].open = true;
            mb->open_index = mb->menu_count;
            mb->sub_item = sub; /* the same menu: its submenu stays */
        }
        mb->menu_count++;
    }
}

static bool s_sw_on; /* the Alt+Tab list is up (below) */
static void (*s_overlay)(void *);
static void *s_overlay_user;

static void draw_taskbar(void) {
    int taskbar_x = 0;
    for (int i = 0; i < win_count; i++) {
        window *win = win_stack[i];
        if (!win->visible || !win->minimized)
            continue;
        int lw = taskbar_label_width(win);
        int ty = SCREEN_HEIGHT - TASKBAR_H;
        fill_rect(taskbar_x, ty, lw, TASKBAR_H, win->bar_color);
        draw_rect(taskbar_x, ty, lw, TASKBAR_H, BLACK);
        draw_string(taskbar_x + 4, ty + 2, (char *)win->title, win->title_color,
                    UI_FONT_BOLD);
        taskbar_x += lw + 1;
    }
}

void wm_draw_all(void) {
    for (int i = 0; i < win_count; i++) {
        window *win = win_stack[i];
        if (!win->visible)
            continue;

        bool animating = win->anim_state != WIN_ANIM_NONE &&
                         win->animate_open_close && !g_cfg.reduce_motion;

        if (win->minimized && !animating)
            continue;

        if (animating) {
            win->anim_frame++;

            if (win->anim_frame >= win->anim_no_of_frames) {
                if (win->anim_state == WIN_ANIM_MINIMIZE) {
                    win->anim_state = WIN_ANIM_NONE;
                    win->minimized = true;
                    continue;
                } else if (win->anim_state == WIN_ANIM_CLOSE) {
                    win->anim_state = WIN_ANIM_NONE;
                    wm_close(win);
                    continue;
                } else {
                    win->anim_state = WIN_ANIM_NONE;
                }
            } else {
                draw_anim(win);
                continue;
            }
        }

        if (win->anim_state != WIN_ANIM_NONE) {
            if (win->anim_state == WIN_ANIM_MINIMIZE) {
                win->anim_state = WIN_ANIM_NONE;
                win->minimized = true;
                continue;
            } else if (win->anim_state == WIN_ANIM_CLOSE) {
                win->anim_state = WIN_ANIM_NONE;
                wm_close(win);
                continue;
            }
            win->anim_state = WIN_ANIM_NONE;
        }

        if (!win->minimized)
            window_draw(win);
    }

    draw_taskbar();

    if (s_overlay) { /* e.g. an icon being dragged between windows */
        gfx_reset_clip();
        s_overlay(s_overlay_user);
    }
}

void wm_set_overlay(void (*fn)(void *user), void *user) {
    s_overlay = fn;
    s_overlay_user = user;
}

void wm_clear_overlay(void *user) {
    if (s_overlay_user == user)
        s_overlay = NULL;
}

/* the window a click of this frame went down on, as things were then */
static window *s_click_win;

window *wm_click_window(void) { return s_click_win; }

window *wm_window_at(int mx, int my) {
    for (int i = win_count - 1; i >= 0; i--) {
        window *w = win_stack[i];
        if (!w->visible || w->minimized)
            continue;
        int wy = w->y + MENUBAR_H;
        if (mx >= w->x && mx <= w->x + w->w && my >= wy &&
            my <= wy + win_shown_h(w))
            return w;
    }
    return NULL;
}

static void finish_closes(void);

void wm_update_all(void) {
    bool click_consumed = false;
    finish_closes();
    /* before the window acts on it (a button may close or minimize it,
     * and the click must not then land on what was under it) */
    s_click_win = mouse.left_clicked || mouse.right_clicked
                      ? wm_window_at(mouse.x, mouse.y)
                      : NULL;

    if (mouse.left_clicked && mouse.y >= SCREEN_HEIGHT - TASKBAR_H) {
        int tx = 0;
        for (int i = 0; i < win_count; i++) {
            window *win = win_stack[i];
            if (!win->visible || !win->minimized)
                continue;
            int lw = taskbar_label_width(win);
            if (mouse.x >= tx && mouse.x < tx + lw) {
                win->minimized = false;
                wm_focus(win);
                if (win->animate_open_close && !g_cfg.reduce_motion) {
                    win->anim_state = WIN_ANIM_RESTORE;
                    win->anim_frame = 0;
                }
                click_consumed = true;
                break;
            }
            tx += lw + 1;
        }
        if (click_consumed)
            return;
    }

    if (mouse.left_clicked && mouse.y < MENUBAR_H)
        return;

    if (!g_menubar_click_consumed) {
        for (int i = win_count - 1; i >= 0; i--) {
            window *win = win_stack[i];
            if (!win->visible || win->minimized)
                continue;
            int wy = win->y + MENUBAR_H;
            bool on_win = mouse.x >= win->x && mouse.x <= win->x + win->w &&
                          mouse.y >= wy && mouse.y <= wy + win_shown_h(win);
            if (!click_consumed && mouse.left_clicked && on_win) {
                wm_focus(win->modal_child ? win->modal_child : win);
                click_consumed = true;
                break;
            }
        }
    }

    if (focused_window && focused_window->visible && !focused_window->minimized)
        window_update(focused_window);
}

/* Keep a window inside the desktop. Apps lay out controls for one size;
 * on a 320x200 screen the window shrinks after the controls exist so the
 * anchor/stretch styles adapt them. */
static void fit_to_screen(window *win) {
    int maxw = SCREEN_WIDTH, maxh = SCREEN_HEIGHT - MENUBAR_H;
    if (win->w > maxw)
        win->w = maxw;
    if (win->h > maxh)
        win->h = maxh;
    if (win->min_w > win->w)
        win->min_w = win->w;
    if (win->min_h > win->h)
        win->min_h = win->h;
    if (win->x + win->w > maxw)
        win->x = maxw - win->w;
    if (win->y + win->h > maxh)
        win->y = maxh - win->h;
    if (win->x < 0)
        win->x = 0;
    if (win->y < 0)
        win->y = 0;
}

static void call_on_draw(void *w, void *unused) {
    (void)unused;
    window *win = (window *)w;
    win->on_draw(win, win->on_draw_userdata);
}

void window_draw(window *win) {
    if (!win || !win->visible || win->minimized)
        return;
    if (win->fit_pending) {
        win->fit_pending = false;
        fit_to_screen(win);
    }

    int wy = win->y + MENUBAR_H;

    int sh = win_shown_h(win);

    if (win->visible_buttons) {
        fill_rect(win->x, wy, win->w, 16, win->bar_color);
        draw_titlebar_btn(win->x + 3, wy + 3, 10, 10, "X", RED);
        draw_titlebar_btn(win->x + 16, wy + 3, 10, 10, "_", LIGHT_BLUE);

        const char *title = win->title ? win->title : "";
        char title_buf[256], star_buf[256];
        int font = UI_FONT_BOLD;
        if (win->unsaved && win->save_changes) { /* "*Name": not saved */
            snprintf(star_buf, sizeof(star_buf), "*%s", title);
            title = star_buf;
        }
        int button_offset = 28;
        int max_width = win->w - button_offset - 6;

        if (text_width(title, font) > max_width) { /* "Long name..." */
            int keep =
                text_fit(title, max_width - text_width("...", font), font);
            if (keep > (int)sizeof(title_buf) - 4)
                keep = (int)sizeof(title_buf) - 4;
            strncpy(title_buf, title, keep);
            title_buf[keep] = '\0';
            strcat(title_buf, "...");
            title = title_buf;
        }

        /* centred on the whole bar, as on Windows 3.1 and the Mac;
         * pushed right only if it would run into the buttons */
        int tx = win->x + (win->w - text_width(title, font)) / 2;
        if (tx < win->x + button_offset + 3)
            tx = win->x + button_offset + 3;

        /* 6-row font sits at +6, the 8-row system font a row higher */
        draw_string(tx, wy + (font == FONT_MONO ? 6 : 5), (char *)title,
                    win->title_color, font);

        if (win->shaded) { /* rolled up: the title bar is all there is */
            draw_rect(win->x, wy, win->w, sh, BLACK);
            return;
        }
        fill_rect(win->x, wy + 16, win->w, win->h - 16, win->content_color);
        draw_rect(win->x, wy, win->w, win->h, BLACK);
        draw_line(win->x, win->y + win->h + MENUBAR_H - 1, win->x + win->w - 1,
                  win->y + win->h + MENUBAR_H - 1, BLACK);
    } else if (win->shaded) {
        return;
    }

    /* app painting and controls stay inside the window */
    gfx_set_clip(win->x, wy, win->w, win->h);
    if (win->on_draw) /* app code: guarded (controls.h) */
        ui_guard(win, call_on_draw, win, NULL, "drawing");
    if (win->controls)
        ctl_draw_all(win);
    gfx_reset_clip();

    if (win->resizable) {
        int gx = win->x + win->w - RESIZE_BUTTON - 1;
        int gy = win->y + MENUBAR_H + win->h - RESIZE_BUTTON;
        fill_rect(gx, gy - 1, RESIZE_BUTTON, RESIZE_BUTTON, LIGHT_GRAY);
        draw_rect(gx, gy - 1, RESIZE_BUTTON, RESIZE_BUTTON, DARK_GRAY);
        draw_line(gx + 1, gy + 8, gx + 8, gy + 1, DARK_GRAY);
        draw_line(gx + 4, gy + 8, gx + 8, gy + 4, DARK_GRAY);
    }
}

bool window_update(window *win) {
    if (!win || !win->visible || win->minimized)
        return false;

    if (win->anim_state != WIN_ANIM_NONE)
        return true;

    int wy = win->y + MENUBAR_H;

    // close button
    if (!is_desktop_focused() &&
        clicked_titlebar_btn(win->x + 3, wy + 3, 10, 10)) {
        if (win->animate_open_close && !g_cfg.reduce_motion) {
            win->anim_state = WIN_ANIM_CLOSE;
            win->anim_frame = 0;
        } else {
            wm_close(win);
        }
        return true;
    }

    // minimize button
    if (!is_desktop_focused() &&
        clicked_titlebar_btn(win->x + 16, wy + 3, 10, 10)) {
        if (win->on_minimize) {
            win->on_minimize(win);
        } else if (win->animate_open_close && !g_cfg.reduce_motion) {
            win->anim_state = WIN_ANIM_MINIMIZE;
            win->anim_frame = 0;
        } else {
            win->minimized = true;
        }
        return true;
    }

    // double-click on the title bar: WindowShade
    if (mouse.left_clicked && !g_menubar_click_consumed &&
        !win->pinned_bottom && in_titlebar(win, mouse.x, mouse.y)) {
        uint32_t now = time_millis();
        if (win->title_click_t && now - win->title_click_t < 450) {
            win->title_click_t = 0;
            wm_shade(win, !win->shaded);
            return true;
        }
        win->title_click_t = now ? now : 1;
    }

    if (!win->shaded)
        window_resize(win);
    window_dragged(win);

    return true;
}

void wm_shade(window *win, bool shaded) {
    if (!win || win->pinned_bottom)
        return;
    win->shaded = shaded;
    win->resizing = false;
}

void wm_zoom(window *win) {
    if (!win || !win->resizable)
        return;
    win->shaded = false;
    if (win->zoomed) {
        win->x = win->zx;
        win->y = win->zy;
        win->w = win->zw;
        win->h = win->zh;
        win->zoomed = false;
        return;
    }
    win->zx = win->x;
    win->zy = win->y;
    win->zw = win->w;
    win->zh = win->h;
    int w = SCREEN_WIDTH, h = SCREEN_HEIGHT - MENUBAR_H - TASKBAR_H;
    if (win->max_w > 0 && w > win->max_w)
        w = win->max_w;
    if (win->max_h > 0 && h > win->max_h)
        h = win->max_h;
    win->x = (SCREEN_WIDTH - w) / 2; /* centred if it can't fill */
    win->y = 0;
    win->w = w;
    win->h = h;
    win->zoomed = true;
}

void window_resize(window *win) {
    if (!win || !win->resizable)
        return;

    int gx = win->x + win->w - 10;
    int gy = win->y + MENUBAR_H + win->h - 10;

    if (!win->resizing && mouse.left_clicked && mouse.x >= gx &&
        mouse.x < gx + 10 && mouse.y >= gy && mouse.y < gy + 10) {
        win->resizing = true;
        win->band = g_cfg.outline_drag != 0; /* a frame, not the window */
        win->bx = win->x;
        win->by = win->y;
        win->bw = win->w;
        win->bh = win->h;
    }

    if (win->resizing) {
        /* live: the window itself; outline: the frame's size */
        int *pw = win->band ? &win->bw : &win->w;
        int *ph = win->band ? &win->bh : &win->h;
        if (mouse.left) {
            if (mouse.dx || mouse.dy)
                win->zoomed = false;
            *pw += mouse.dx;
            *ph += mouse.dy;
            int min_w = win->min_w > 0 ? win->min_w : 60;
            int min_h = win->min_h > 0 ? win->min_h : 40;
            if (*pw < min_w)
                *pw = min_w;
            if (*ph < min_h)
                *ph = min_h;
            int max_w = SCREEN_WIDTH - win->x;
            int max_h = SCREEN_HEIGHT - MENUBAR_H - TASKBAR_H - win->y;
            if (*pw > max_w)
                *pw = max_w;
            if (*ph > max_h)
                *ph = max_h;
            if (win->max_w > 0 && *pw > win->max_w)
                *pw = win->max_w;
            if (win->max_h > 0 && *ph > win->max_h)
                *ph = win->max_h;
        } else {
            if (win->band) {
                win->w = win->bw;
                win->h = win->bh;
                win->band = false;
            }
            win->resizing = false;
        }
    }
}

/* where the pointer was when the drag began (one drag at a time) */
static int s_drag_mx, s_drag_my;

void window_dragged(window *win) {
    if (!win || win->pinned_bottom || win->resizing)
        return;

    if (!win->dragging && mouse.left_clicked &&
        in_titlebar(win, mouse.x, mouse.y)) {
        win->dragging = true;
        s_drag_mx = mouse.x;
        s_drag_my = mouse.y;
        win->band = g_cfg.outline_drag != 0; /* a frame, not the window */
        win->bx = win->x;
        win->by = win->y;
        win->bw = win->w;
        win->bh = win_shown_h(win);
    }

    if (win->dragging) {
        if (mouse.left) {
            if (win->band) { /* move the frame, kept on the screen */
                win->bx += mouse.dx;
                win->by += mouse.dy;
                clamp_xy(&win->bx, &win->by, win->w, win->h);
            } else {
                win->x += mouse.dx;
                win->y += mouse.dy;
                wm_clamp(win);
            }
        } else {
            if (win->band) { /* let go: the window goes where the frame is */
                win->x = win->bx;
                win->y = win->by;
                win->band = false;
            }
            /* dragged to the bottom edge = minimize (no auto-tiling); a
             * click on a title bar down there isn't a drag */
            int snap_threshold = 2 * TASKBAR_H;
            int mdx = mouse.x - s_drag_mx, mdy = mouse.y - s_drag_my;
            bool moved = mdx * mdx + mdy * mdy > 4 * 4;

            if (moved && mouse.y > SCREEN_HEIGHT - TASKBAR_H - snap_threshold) {
                win->dragging = false;
                win->minimized = true;
            }

            win->dragging = false;
        }
    }
}

bool wm_is_registered(const window *win) {
    for (int i = 0; i < win_count; i++)
        if (win_stack[i] == win)
            return true;
    return false;
}

/* Close button: the window's own handler, else end the app that owns it,
 * else just hide it. */
static void close_now(window *win) {
    win->close_saving = false;
    if (win->on_close)
        win->on_close(win);
    else if (!os_close_own_instance(win))
        win->visible = false;
}

// asking to save before closing

void wm_ask_save(window *win, bool (*save)(window *win)) {
    if (win)
        win->save_changes = save;
}

void wm_set_unsaved(window *win, bool unsaved) {
    if (win && win->unsaved != unsaved) {
        win->unsaved = unsaved;
        wm_invalidate(win); /* (its title's *) */
    }
}

int wm_unsaved_titles(char *out, int n) {
    int count = 0, o = 0;
    if (n > 0)
        out[0] = 0;
    for (int i = 0; i < win_count; i++) {
        window *w = win_stack[i];
        if (!w->unsaved || !w->save_changes)
            continue;
        if (o < n - 1)
            o += snprintf(out + o, n - o, "%s%s", count ? ", " : "",
                          w->title ? w->title : "");
        count++;
    }
    return count;
}

/* Save, Don't save, Cancel: the answer (run as the app, if it is one) */
static void save_answer(void *user, int choice) {
    window *w = user;
    if (!wm_is_registered(w))
        return;
    if (choice == 0) { /* Save: closes when saved (wm_finish_closes) */
        w->close_saving = true;
        if (w->save_changes)
            w->save_changes(w);
    } else if (choice == 1) { /* Don't save */
        w->unsaved = false;
        close_now(w);
    }
}

void wm_close(window *win) {
    if (!win)
        return;
    if (!win->save_changes || !win->unsaved) {
        close_now(win);
        return;
    }
    if (win->modal_child) { /* a dialog is open: that first */
        wm_focus(win->modal_child);
        return;
    }
    if (win->minimized)
        win->minimized = false;
    wm_focus(win);
    char msg[128];
    snprintf(msg, sizeof(msg), "Save the changes to %s before closing?",
             win->title && win->title[0] ? win->title : "this window");
    static const char *const BUTTONS[3] = {"Save", "Don't save", "Cancel"};
    dlg_ask(win, "Save changes?", msg, BUTTONS, 3, save_answer, win);
}

/* Save was picked: once no dialog is open (a Save As may be), it closes
 * if the changes were saved, else it stays (the Save As was cancelled) */
static void finish_closes(void) {
    for (int i = 0; i < win_count; i++) {
        window *w = win_stack[i];
        if (!w->close_saving || w->modal_child)
            continue;
        w->close_saving = false;
        if (!w->unsaved)
            close_now(w);
        return; /* (one a frame: the stack may have changed) */
    }
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
    m->title = (char *)title;
    m->open = false;
    m->item_count = 0;
    return m;
}

// lazy redraw (see window.h)

uint32_t g_wm_frames[3];

static bool s_force = true;  /* draw everything on the next frame */
static int s_grace;          /* ... and for this many frames after input */
static uint32_t s_scene_sig; /* the scene the back buffer shows */
static uint32_t s_bar_sig;
static bool s_prev_button, s_button;

#define MIX(v) (h = h * 33u ^ (uint32_t)(v))

void wm_invalidate(window *win) {
    if (win)
        win->dirty = true;
}

void wm_invalidate_all(void) { s_force = true; }

void wm_invalidate_app(window *win) {
    if (!win)
        return;
    for (int i = 0; i < win_count; i++) {
        window *w = win_stack[i];
        if (w == win || (win->userdata && w->userdata == win->userdata) ||
            (win->app_instance && w->app_instance == win->app_instance))
            w->dirty = true;
    }
}

/* Input may change app-owned drawing, so redraw those frames fully plus two
 * follow-up frames. Time updates and plain mouse movement use lazy redraw. */
void wm_frame_begin(void) {
    s_button = mouse.left || mouse.right || mouse.middle;
    if (mouse.left_clicked || mouse.right_clicked || kb.key_pressed ||
        (s_prev_button && !s_button)) {
        s_force = true;
        s_grace = 2;
    }
    s_prev_button = s_button;
}

static uint32_t mix_str(uint32_t h, const char *p) {
    if (p)
        for (; *p; p++)
            MIX((uint8_t)*p);
    return h * 33u;
}

/* layout of the whole screen: any change means a full redraw */
static uint32_t scene_sig(const menubar *mb) {
    uint32_t h = 5381;
    MIX(win_count);
    MIX((uintptr_t)focused_window);
    MIX(SCREEN_WIDTH);
    MIX(g_cfg.classic_fonts);
    for (int i = 0; i < win_count; i++) {
        const window *w = win_stack[i];
        MIX((uintptr_t)w);
        MIX(w->x);
        MIX(w->y);
        MIX(w->w);
        MIX(w->h);
        MIX(w->visible | w->minimized << 1 | w->shaded << 2 | w->dragging << 3 |
            w->resizing << 4 | w->pinned_bottom << 5 | w->visible_buttons << 6 |
            w->resizable << 7 | w->anim_state << 8 | w->anim_frame << 16);
        MIX((uintptr_t)w->modal_child);
        if (w->visible && w->minimized) { /* its taskbar label */
            h = mix_str(h, w->title);
            MIX(w->bar_color << 8 | w->title_color);
        }
    }
    MIX(mb->open_index);
    MIX(mb->sub_item);
    MIX(mb->first);
    MIX(popup_is_open() | ctl_overlay_active() << 1 | (s_overlay != NULL) << 2);
    MIX(desktop_sig());
    return h;
}

/* things that move every frame or are drawn over the windows */
static bool scene_transient(const menubar *mb) {
    if (mb->open_index >= 0 || popup_is_open() || ctl_overlay_active() ||
        s_overlay || s_sw_on)
        return true;
    for (int i = 0; i < win_count; i++) {
        const window *w = win_stack[i];
        if (w->visible && (w->anim_state != WIN_ANIM_NONE || w->fit_pending ||
                           ((w->dragging || w->resizing) && !w->band)))
            return true;
    }
    return false;
}

/* what one window shows */
static uint32_t window_sig(window *w, bool *custom) {
    uint32_t h = ctl_window_sig(w, custom);
    if (w->on_draw)
        *custom = true;
    h = mix_str(h, w->title);
    MIX(w->bar_color << 16 | w->title_color << 8 | w->content_color);
    MIX((uintptr_t)w->focus);
    return h;
}

static uint32_t bar_sig(const menubar *mb) {
    uint32_t h = 5381;
    char clock[9];
    menubar_clock_text(clock); /* what the clock shows */
    h = mix_str(h, clock);
    MIX(mb->menu_count);
    for (int i = 0; i < mb->menu_count; i++)
        MIX((uintptr_t)mb->menus[i].title);
    MIX(mb->first);
    return h;
}

static window *window_under_mouse(void) {
    for (int i = win_count - 1; i >= 0; i--) {
        window *w = win_stack[i];
        if (!w->visible || w->minimized)
            continue;
        int wy = w->y + MENUBAR_H;
        if (mouse.x >= w->x && mouse.x < w->x + w->w && mouse.y >= wy &&
            mouse.y < wy + win_shown_h(w))
            return w;
    }
    return NULL;
}

// the outline frame (outline move/resize)
/*
 * XORed into the back buffer, so drawing it again takes it away: no
 * redraw of what is under it while it moves. */
static bool s_band_on; /* drawn in the back buffer now */
static int s_bx, s_by, s_bw, s_bh;

static bool band_now(int *x, int *y, int *w, int *h) {
    for (int i = 0; i < win_count; i++) {
        const window *b = win_stack[i];
        if (b->visible && b->band && (b->dragging || b->resizing)) {
            *x = b->bx;
            *y = b->by + MENUBAR_H;
            *w = b->bw;
            *h = b->bh;
            return true;
        }
    }
    return false;
}

static void xor_hline(int x0, int x1, int y) {
    if (y < MENUBAR_H || y >= SCREEN_HEIGHT)
        return;
    if (x0 < 0)
        x0 = 0;
    if (x1 >= SCREEN_WIDTH)
        x1 = SCREEN_WIDTH - 1;
    for (int x = x0; x <= x1; x++)
        BACKBUF[y * SCREEN_WIDTH + x] ^= 0x0F;
}

static void xor_vline(int x, int y0, int y1) {
    if (x < 0 || x >= SCREEN_WIDTH)
        return;
    if (y0 < MENUBAR_H)
        y0 = MENUBAR_H;
    if (y1 >= SCREEN_HEIGHT)
        y1 = SCREEN_HEIGHT - 1;
    for (int y = y0; y <= y1; y++)
        BACKBUF[y * SCREEN_WIDTH + x] ^= 0x0F;
}

/* a 2-pixel frame; every pixel exactly once, so it undoes itself */
static void xor_frame(int x, int y, int w, int h) {
    for (int k = 0; k < 2 && w - 2 * k > 0 && h - 2 * k > 0; k++) {
        int x0 = x + k, y0 = y + k, x1 = x + w - 1 - k, y1 = y + h - 1 - k;
        xor_hline(x0, x1, y0);
        if (y1 > y0)
            xor_hline(x0, x1, y1);
        if (y1 - y0 > 1) {
            xor_vline(x0, y0 + 1, y1 - 1);
            if (x1 > x0)
                xor_vline(x1, y0 + 1, y1 - 1);
        }
    }
}

// Alt+Tab
/*
 * Hold Alt, press Tab (Shift+Tab backwards) to walk the windows, most
 * recently used first; let go of Alt to switch, Esc to stay. A box in the
 * middle of the screen lists them. Dialogs go with their window. */
#define SW_MAX 16
static window *s_sw[SW_MAX];
static int s_sw_n, s_sw_sel;

static window *top_level(window *w) {
    while (w && w->modal_owner)
        w = w->modal_owner;
    return w;
}

static void sw_collect(void) {
    /* most recent first: a window counts from its highest dialog too */
    s_sw_n = 0;
    window *cur = top_level(wm_focused_window());
    if (cur && !cur->pinned_bottom)
        s_sw[s_sw_n++] = cur;
    for (int i = win_count - 1; i >= 0 && s_sw_n < SW_MAX; i--) {
        window *w = top_level(win_stack[i]);
        if (!w || !w->visible || w->pinned_bottom ||
            w->anim_state == WIN_ANIM_CLOSE)
            continue;
        bool have = false;
        for (int k = 0; k < s_sw_n; k++)
            have |= s_sw[k] == w;
        if (!have)
            s_sw[s_sw_n++] = w;
    }
}

static void sw_finish(bool go) {
    s_sw_on = false;
    s_force = true;
    if (!go || s_sw_sel < 0 || s_sw_sel >= s_sw_n)
        return;
    window *w = s_sw[s_sw_sel];
    if (!wm_is_registered(w))
        return; /* closed while the box was up */
    if (w->minimized) {
        w->minimized = false;
        if (w->animate_open_close && !g_cfg.reduce_motion) {
            w->anim_state = WIN_ANIM_RESTORE;
            w->anim_frame = 0;
        }
    }
    wm_focus(w);
    window *d = w->modal_child; /* its dialog on top of it */
    while (d && d->modal_child)
        d = d->modal_child;
    if (d)
        wm_focus(d);
}

bool wm_switcher_key(void) {
    if (s_sw_on && !kb.alt) { /* Alt let go: switch */
        sw_finish(true);
        return false;
    }
    if (!kb.key_pressed)
        return s_sw_on;
    uint8_t sc = kb.last_scancode;
    if (kb.alt && !kb.ctrl && sc == TAB) {
        kb.key_pressed = false;
        kb.last_char = 0;
        if (!s_sw_on) {
            sw_collect();
            if (s_sw_n < 2 && !(s_sw_n == 1 && s_sw[0]->minimized))
                return false; /* nothing to switch to */
            s_sw_on = true;
            s_sw_sel = 0;
        }
        if (s_sw_n > 0)
            s_sw_sel = (s_sw_sel + (kb.shift ? s_sw_n - 1 : 1)) % s_sw_n;
        s_force = true;
        return true;
    }
    if (s_sw_on) { /* other keys while the box is up */
        kb.key_pressed = false;
        kb.last_char = 0;
        if (sc == ESC)
            sw_finish(false);
        else if (sc == ENTER)
            sw_finish(true);
        return true;
    }
    return false;
}

bool wm_switcher_active(void) { return s_sw_on; }

static void sw_draw(void) {
    if (!s_sw_on)
        return;
    int f = UI_FONT_SMALL;
    int rh = MENU_ITEM_H;
    int w = 120;
    for (int i = 0; i < s_sw_n; i++) {
        int tw = text_width(s_sw[i]->title ? s_sw[i]->title : "", f) + 16;
        if (tw > w)
            w = tw;
    }
    if (w > SCREEN_WIDTH - 20)
        w = SCREEN_WIDTH - 20;
    int head = rh + 4;
    int h = head + s_sw_n * rh + 4;
    int x = (SCREEN_WIDTH - w) / 2, y = (SCREEN_HEIGHT - h) / 2;
    gfx_reset_clip();
    fill_rect(x + 2, y + 2, w, h, BLACK);
    fill_rect(x, y, w, h, CYAN);
    draw_rect(x, y, w, h, BLACK);
    draw_string(x + 6, y + 3, "Switch to:", WHITE, UI_FONT_BOLD);
    draw_line(x + 3, y + head - 1, x + w - 4, y + head - 1, WHITE);
    gfx_set_clip(x + 1, y + 1, w - 2, h - 2);
    for (int i = 0; i < s_sw_n; i++) {
        int ry = y + head + 1 + i * rh;
        if (i == s_sw_sel)
            fill_rect(x + 1, ry, w - 2, rh, RED);
        const char *t = s_sw[i]->title ? s_sw[i]->title : "(untitled)";
        draw_string(x + 8, ry + 2, t, s_sw[i]->minimized ? LIGHT_GRAY : WHITE,
                    f);
    }
    gfx_reset_clip();
}

static void draw_scene(menubar *mb) {
    gfx_reset_clip();
    desktop_draw();
    wm_draw_all();
    menubar_draw(mb);
    popup_draw();
    ctl_draw_tooltip();
    sw_draw();
}

int wm_compose(menubar *mb, int *y0, int *y1) {
    uint32_t sig = scene_sig(mb);
    bool full =
        s_force || s_grace > 0 || sig != s_scene_sig || scene_transient(mb);
    if (s_grace > 0)
        s_grace--;
    s_force = false;
    s_scene_sig = sig;
    uint32_t bar = bar_sig(mb);

    /* which windows need drawing: their content changed, or a window
     * below them is drawn (it would paint over them) */
    static bool draw[MAX_WINDOWS];
    static uint32_t sigs[MAX_WINDOWS];
    bool any = false;
    int bx = 0, by = 0, bw = 0, bh = 0;
    bool band = band_now(&bx, &by, &bw, &bh);
    bool painting = s_button && !band; /* the button is held over an app */
    window *under = painting ? window_under_mouse() : NULL;
    for (int i = 0; i < win_count; i++) {
        window *w = win_stack[i];
        draw[i] = false;
        if (!w->visible || w->minimized || w->pinned_bottom)
            continue;
        bool custom;
        sigs[i] = window_sig(w, &custom);
        if (full)
            continue;
        bool need = w->dirty || sigs[i] != w->sig ||
                    (custom && !w->lazy_custom) ||
                    (painting && (w == under || w == focused_window));
        for (int k = 0; k < i && !need; k++) {
            const window *b = win_stack[k];
            need = draw[k] && b->x < w->x + w->w && w->x < b->x + b->w &&
                   b->y < w->y + win_shown_h(w) && w->y < b->y + win_shown_h(b);
        }
        if (need && !w->visible_buttons)
            full = true; /* no frame of its own: not opaque */
        draw[i] = need;
        any |= need;
    }

    if (full) {
        /* clean before drawing: a paint may ask for the next frame */
        for (int i = 0; i < win_count; i++) {
            win_stack[i]->sig = sigs[i];
            win_stack[i]->dirty = false;
        }
        draw_scene(mb);
        s_band_on = band;
        if (band) {
            xor_frame(bx, by, bw, bh);
            s_bx = bx, s_by = by, s_bw = bw, s_bh = bh;
        }
        s_bar_sig = bar;
        *y0 = 0;
        *y1 = SCREEN_HEIGHT - 1;
        g_wm_frames[WM_FRAME_FULL]++;
        return WM_FRAME_FULL;
    }

    bool bar_dirty = bar != s_bar_sig;
    bool band_dirty =
        band != s_band_on ||
        (band && (bx != s_bx || by != s_by || bw != s_bw || bh != s_bh));
    /* (the pointer is not part of the frame: drivers/pointer.h) */
    if (!any && !bar_dirty && !band_dirty) {
        g_wm_frames[WM_FRAME_NONE]++;
        return WM_FRAME_NONE; /* the screen is already right */
    }

    int lo = SCREEN_HEIGHT, hi = -1;
#define DAMAGE(y, h)                                                           \
    do {                                                                       \
        if ((y) < lo)                                                          \
            lo = (y);                                                          \
        if ((y) + (h) - 1 > hi)                                                \
            hi = (y) + (h) - 1;                                                \
    } while (0)
    /* the frame comes off before anything is drawn (drawing over it would
     * spoil the XOR), and goes back on after */
    if (s_band_on && (band_dirty || any || bar_dirty)) {
        xor_frame(s_bx, s_by, s_bw, s_bh);
        DAMAGE(s_by, s_bh);
        s_band_on = false;
    }
    if (any) {
        for (int i = 0; i < win_count; i++) {
            if (!draw[i])
                continue;
            window *w = win_stack[i];
            w->sig = sigs[i];
            w->dirty = false;
            gfx_reset_clip();
            window_draw(w);
            DAMAGE(w->y + MENUBAR_H, win_shown_h(w));
        }
        gfx_reset_clip();
        draw_taskbar(); /* a tall window can reach under it */
        DAMAGE(SCREEN_HEIGHT - TASKBAR_H, TASKBAR_H);
    }
    if (bar_dirty) {
        gfx_reset_clip();
        menubar_draw(mb);
        s_bar_sig = bar;
        DAMAGE(0, MENUBAR_H);
    }
    if (band && !s_band_on) {
        xor_frame(bx, by, bw, bh);
        DAMAGE(by, bh);
        s_band_on = true;
        s_bx = bx, s_by = by, s_bw = bw, s_bh = bh;
    }
#undef DAMAGE
    *y0 = lo < 0 ? 0 : lo;
    *y1 = hi >= SCREEN_HEIGHT ? SCREEN_HEIGHT - 1 : hi;
    g_wm_frames[WM_FRAME_PART]++;
    return WM_FRAME_PART;
}

/* Tests: draw the scene the plain way (nothing lazy) to compare with what
 * the lazy path left in the back buffer. False when it can't be compared
 * (something is animating). */
bool wm_debug_reference(menubar *mb) {
    if (scene_transient(mb))
        return false;
    draw_scene(mb);
    int bx, by, bw, bh;
    if (band_now(&bx, &by, &bw, &bh))
        xor_frame(bx, by, bw, bh);
    return true;
}
