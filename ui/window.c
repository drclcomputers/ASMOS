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

static void clamp_xy(int *x, int *y, int w, int h) {
    if (*x < 0)
        *x = 0;
    if (*y < 0)
        *y = 0;
    if (*x + w > SCREEN_WIDTH)
        *x = SCREEN_WIDTH - w;
    if (*y + h + MENUBAR_H + TASKBAR_H > SCREEN_HEIGHT)
        *y = SCREEN_HEIGHT - h - MENUBAR_H - TASKBAR_H;
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
        ERR_WARN_REPORT(ERR_WM_ALLOC, "wm_register: kmalloc");
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

void wm_unregister(window *win) {
    if (!win || !wm_is_registered(win))
        return;
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
    mb->menu_count = 0;
    mb->open_index = -1;

    if (!fw || fw->win_menu_count == 0)
        return;

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

void wm_update_all(void) {
    bool click_consumed = false;

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

        const char *title = win->title;
        char title_buf[256];
        int font = UI_FONT_BOLD;
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
        fill_rect(gx, gy, RESIZE_BUTTON, RESIZE_BUTTON, LIGHT_GRAY);
        draw_rect(gx, gy, RESIZE_BUTTON, RESIZE_BUTTON, DARK_GRAY);
        draw_line(gx + 1, gy + 6, gx + 6, gy + 1, DARK_GRAY);
        draw_line(gx + 4, gy + 6, gx + 6, gy + 4, DARK_GRAY);
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

void window_dragged(window *win) {
    if (!win || win->pinned_bottom || win->resizing)
        return;

    if (!win->dragging && mouse.left_clicked &&
        in_titlebar(win, mouse.x, mouse.y)) {
        win->dragging = true;
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
            /* drag to the bottom edge = minimize (no auto-tiling) */
            int snap_threshold = 2 * TASKBAR_H;

            if (mouse.y > SCREEN_HEIGHT - TASKBAR_H - snap_threshold) {
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
void wm_close(window *win) {
    if (win->on_close)
        win->on_close(win);
    else if (!os_close_own_instance(win))
        win->visible = false;
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

/* ── lazy redraw (see window.h) ────────────────────────────────────────── */

uint32_t g_wm_frames[3];

static bool s_force = true;  /* draw everything on the next frame */
static int s_grace;          /* ... and for this many frames after input */
static uint32_t s_scene_sig; /* the scene the back buffer shows */
static uint32_t s_bar_sig;
static bool s_prev_button, s_button;
static int s_cur_x = -1, s_cur_y = -1, s_cur_shape = -1;

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

/* Clicks, releases and keys can change anything (and apps read them on
 * their own), so those frames are drawn in full, plus two more for work
 * an app does a frame later. Only time and plain mouse movement go the
 * lazy way. */
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
    MIX(popup_is_open() | ctl_overlay_active() << 1 | (s_overlay != NULL) << 2);
    MIX(desktop_sig());
    return h;
}

/* things that move every frame or are drawn over the windows */
static bool scene_transient(const menubar *mb) {
    if (mb->open_index >= 0 || popup_is_open() || ctl_overlay_active() ||
        s_overlay)
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

/* ── the outline frame (outline move/resize) ─────────────────────────────
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

static void draw_scene(menubar *mb) {
    gfx_reset_clip();
    desktop_draw();
    wm_draw_all();
    menubar_draw(mb);
    popup_draw();
    ctl_draw_tooltip();
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
        cursor_forget();
        draw_scene(mb);
        s_band_on = band;
        if (band) {
            xor_frame(bx, by, bw, bh);
            s_bx = bx, s_by = by, s_bw = bw, s_bh = bh;
        }
        cursor_show(mouse.x, mouse.y);
        s_bar_sig = bar;
        s_cur_x = mouse.x;
        s_cur_y = mouse.y;
        s_cur_shape = CHOSEN_CURSOR;
        *y0 = 0;
        *y1 = SCREEN_HEIGHT - 1;
        g_wm_frames[WM_FRAME_FULL]++;
        return WM_FRAME_FULL;
    }

    bool bar_dirty = bar != s_bar_sig;
    bool cur_dirty = mouse.x != s_cur_x || mouse.y != s_cur_y ||
                     CHOSEN_CURSOR != s_cur_shape;
    bool band_dirty =
        band != s_band_on ||
        (band && (bx != s_bx || by != s_by || bw != s_bw || bh != s_bh));
    if (!any && !bar_dirty && !cur_dirty && !band_dirty) {
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
    int cy, ch;
    if (cursor_hide(&cy, &ch))
        DAMAGE(cy, ch);
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
    cursor_show(mouse.x, mouse.y);
    DAMAGE(mouse.y, CURSOR_H_PX);
    s_cur_x = mouse.x;
    s_cur_y = mouse.y;
    s_cur_shape = CHOSEN_CURSOR;
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
    gfx_reset_clip();
    draw_cursor(mouse.x, mouse.y);
    return true;
}
