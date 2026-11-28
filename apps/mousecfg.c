// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* Mouse: the pointer's speed, how quick a double-click must be (with a box
 * to try it on), the buttons swapped. Opened from Settings > Devices (a
 * double-click on the mouse), not the Apps menu. Each change works at
 * once; OK keeps them (ASMOS.CFG), Cancel puts back the old ones. */

#include "config/runtime_config.h"
#include "drivers/mouse.h"
#include "drivers/mousemove.h"
#include "drivers/sermouse.h"
#include "os/api.h"
#include "ui/uifont.h"

typedef struct {
    window *win; /* must stay first */
    control *speed, *dbl, *swap, *test, *dbl_lbl;
    mouse_cfg_t saved; /* at the start: Cancel puts it back */
    bool lit;          /* the test box: double-clicked */
    uint32_t lit_at;
} mouseapp_t;

app_descriptor mousecfg_app;

static mouseapp_t *me(control *c) { return c ? c->win->userdata : NULL; }

// the test box

static void paint_test(control *c, int x, int y, int w, int h) {
    mouseapp_t *s = me(c);
    bool lit = s && s->lit;
    fill_rect(x, y, w, h, lit ? LIGHT_GREEN : WHITE);
    const char *t = lit ? "A double-click!" : "Double-click here to try it";
    int f = UI_FONT_SMALL;
    draw_string(x + (w - text_width(t, f)) / 2, y + (h - 7) / 2, t,
                lit ? BLACK : DARK_GRAY, f);
}

static void on_test(control *c, int ev) {
    mouseapp_t *s = me(c);
    if (!s)
        return;
    if (ev == EV_DBLCLICK) { /* (quick enough for the setting, as it is) */
        s->lit = !s->lit;
        s->lit_at = time_millis();
        wm_invalidate(c->win);
    }
}

// the settings

static void show_dbl(mouseapp_t *s) {
    char t[24];
    snprintf(t, sizeof(t), "%u ms",
             (unsigned)mouse_dblclick_ms_at(ctl_value(s->dbl)));
    ctl_set_text(s->dbl_lbl, t);
}

static void on_speed(control *c, int ev) {
    if (ev == EV_CHANGE)
        g_cfg.mouse.speed = (uint8_t)ctl_value(c);
}

static void on_dbl(control *c, int ev) {
    mouseapp_t *s = me(c);
    if (ev != EV_CHANGE || !s)
        return;
    g_cfg.mouse.dbl = (uint8_t)ctl_value(c);
    show_dbl(s);
}

static void on_swap(control *c, int ev) {
    if (ev == EV_CHANGE) /* (from the next click: the other button) */
        g_cfg.mouse.swap = ctl_value(c) != 0;
}

static void set_controls(mouseapp_t *s) {
    ctl_set_value(s->speed, g_cfg.mouse.speed ? g_cfg.mouse.speed : 5);
    ctl_set_value(s->dbl, g_cfg.mouse.dbl ? g_cfg.mouse.dbl : 5);
    ctl_set_value(s->swap, g_cfg.mouse.swap);
    show_dbl(s);
}

static void on_defaults(control *c, int ev) {
    mouseapp_t *s = me(c);
    if (ev != EV_CLICK || !s)
        return;
    g_cfg.mouse.speed = g_cfg.mouse.dbl = 5;
    g_cfg.mouse.swap = 0;
    set_controls(s);
}

static void m_close(void) { os_quit_app_by_desc(&mousecfg_app); }

static void on_ok(control *c, int ev) {
    (void)c;
    if (ev != EV_CLICK)
        return;
    cfg_save(); /* (false from the setup floppy: it stays until restart) */
    m_close();
}

/* Cancel, Esc, the X and Ctrl+Q: the settings it started with */
static bool on_window_close(window *w) {
    mouseapp_t *s = w->userdata;
    if (s)
        g_cfg.mouse = s->saved;
    m_close();
    return true;
}

static void on_cancel(control *c, int ev) {
    if (ev == EV_CLICK)
        on_window_close(c->win);
}

/* what it is plugged into */
static void connection(char *out, int sz) {
    mouse_kind_t mk = mouse_kinds();
    if ((mk & MOUSE_PS2) && (mk & MOUSE_SERIAL))
        snprintf(out, sz, "Mice on the PS/2 port and COM%d", sermouse_port());
    else if (mk & MOUSE_SERIAL)
        snprintf(out, sz, "A serial mouse on COM%d", sermouse_port());
    else if (mk & MOUSE_PS2)
        snprintf(out, sz, "A mouse on the PS/2 port");
    else
        snprintf(out, sz, "No mouse found");
}

static void mousecfg_init(void *state) {
    mouseapp_t *s = state;
    s->win = app_window(s, "Mouse", 40, 16, 236, 156, DARK_GRAY, 0);
    if (!s->win)
        return;
    window *w = s->win;
    w->userdata = s;
    s->saved = g_cfg.mouse;
    w->on_close = on_window_close;

    ctl_label(w, 8, 6, 220, 10, "Pointer speed", 0);
    ctl_label(w, 8, 20, 26, 10, "Slow", 0);
    s->speed = ctl_slider(w, 36, 19, 150, 1, MOUSE_SPEEDS, 5, on_speed);
    ctl_set_tooltip(s->speed, "How far the pointer goes for a move");
    ctl_label(w, 190, 20, 30, 10, "Fast", 0);

    ctl_label(w, 8, 38, 120, 10, "Double-click speed", 0);
    s->dbl_lbl = ctl_label(w, 150, 38, 70, 10, "", 0);
    ctl_label(w, 8, 52, 26, 10, "Slow", 0);
    s->dbl = ctl_slider(w, 36, 51, 150, 1, MOUSE_SPEEDS, 5, on_dbl);
    ctl_set_tooltip(s->dbl, "How quick the two clicks of a double-click "
                            "must be");
    ctl_label(w, 190, 52, 30, 10, "Fast", 0);
    s->test = ctl_custom(w, 8, 66, 220, 20, paint_test, on_test, CS_BORDER);

    s->swap =
        ctl_check(w, 8, 94, 220, "Swap the left and right buttons", 0, on_swap);
    ctl_set_tooltip(s->swap, "For the left hand: the right button clicks");
    char t[64];
    connection(t, sizeof(t));
    control *k = ctl_label(w, 8, 110, 220, 10, t, 0);
    ctl_set_colors(k, DARK_GRAY, LIGHT_GRAY);

    ctl_button(w, 8, 122, 52, 13, "Defaults", 0, on_defaults);
    control *ok = ctl_button(w, 132, 122, 44, 13, "OK", 1, on_ok);
    ctl_style_add(ok, CS_DEFAULT);
    control *cn = ctl_button(w, 180, 122, 48, 13, "Cancel", 1, on_cancel);
    ctl_style_add(cn, CS_CANCEL);
    set_controls(s);

    menu *f = window_add_menu(w, "Mouse");
    menu_add_item(f, "Close", m_close);
}

static void mousecfg_frame(void *state) {
    mouseapp_t *s = state;
    if (s->win && s->lit && time_millis() - s->lit_at > 2000) {
        s->lit = false;
        wm_invalidate(s->win);
    }
}

app_descriptor mousecfg_app = {
    .name = "MOUSE",
    .state_size = sizeof(mouseapp_t),
    .init = mousecfg_init,
    .on_frame = mousecfg_frame,
    .single_instance = true,
};
