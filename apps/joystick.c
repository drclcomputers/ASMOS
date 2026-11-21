/* Joystick - set up the game port's stick (drivers/joystick.h): a live view,
 * calibration, dead zone and reversed axes. It opens from Settings >
 * Devices; OK keeps the settings in ASMOS.CFG, Cancel puts back the ones
 * it started with. */
#include "drivers/joystick.h"
#include "config/runtime_config.h"
#include "os/api.h"
#include "ui/uifont.h"

typedef struct {
    window *win; /* must stay first */
    control *dz, *dz_lbl, *inv[JOY_AXES];
    control *cal, *reset, *detect, *ok, *msg;
    int step;           /* 0 idle, 1 centre, 2 range */
    bool shown_present; /* what the controls were enabled for */
    bool range_short;   /* a Next at step 2 found an axis too short */
    joy_cfg_t saved;    /* at the start: Cancel puts it back */
} joyapp_t;

app_descriptor joyconf_app;

static const char AXIS_NAME[JOY_AXES] = {'X', 'Y', 'Z', 'R'};

// the live view

/* a square pad: the stick as a dot, the dead zone as a frame */
static void pad(int x, int y, int sz, int ax, int ay, int dz, bool live) {
    fill_rect(x, y, sz, sz, BLACK);
    draw_rect(x, y, sz, sz, DARK_GRAY);
    if (!live) { /* no such axes: an empty frame */
        draw_string(x + sz / 2 - 10, y + sz / 2 - 3, "none", DARK_GRAY,
                    UI_FONT_SMALL);
        return;
    }
    int c = sz / 2, half = c - 4;
    draw_line(x + c, y + 1, x + c, y + sz - 2, DARK_GRAY);
    draw_line(x + 1, y + c, x + sz - 2, y + c, DARK_GRAY);
    int d = half * dz / 100;
    if (d > 0)
        draw_rect(x + c - d, y + c - d, 2 * d + 1, 2 * d + 1, BLUE);
    fill_rect(x + c + ax * half / 100 - 2, y + c + ay * half / 100 - 2, 5, 5,
              LIGHT_GREEN);
}

static void paint_view(control *c, int x, int y, int w, int h) {
    (void)c;
    int f = UI_FONT_SMALL;
    fill_rect(x, y, w, h, BLACK);
    if (!joy.present) {
        const char *m = "No joystick found";
        draw_string(x + (w - text_width(m, f)) / 2, y + h / 2 - 3, m,
                    LIGHT_GRAY, f);
        return;
    }
    int dz = g_cfg.joy.deadzone;
    pad(x + 4, y + 4, 70, joy.axis[0], joy.axis[1], dz, true);
    pad(x + 78, y + 4, 70, joy.axis[2], joy.axis[3], dz,
        (joy.axes & 0x0C) != 0);
    int tx = x + 154;
    for (int i = 0; i < JOY_AXES; i++) {
        char t[32];
        bool on = joy.axes & (1 << i);
        if (on)
            snprintf(t, sizeof(t), "%c %4d  %u", AXIS_NAME[i], joy.axis[i],
                     (unsigned)joy_raw(i));
        else
            snprintf(t, sizeof(t), "%c    -", AXIS_NAME[i]);
        draw_string(tx, y + 4 + i * 9, t, on ? WHITE : DARK_GRAY, f);
    }
    draw_string(tx, y + 44, "Buttons", LIGHT_GRAY, f);
    for (int i = 0; i < JOY_BUTTONS; i++) {
        bool down = joy.buttons & (1 << i);
        fill_rect(tx + i * 17, y + 54, 13, 11, down ? LIGHT_RED : DARK_GRAY);
        char d[2] = {(char)('1' + i), 0};
        draw_string(tx + i * 17 + 5, y + 57, d, down ? WHITE : LIGHT_GRAY, f);
    }
}

// the controls

static void say(joyapp_t *s, const char *t) { ctl_set_text(s->msg, t); }

static void idle_message(joyapp_t *s) {
    if (!joy.present)
        say(s, "No joystick found. Plug it into the game port,\n"
               "then press Detect.");
    else if (!g_cfg.joy.calibrated)
        say(s, "Not calibrated: the stick is taken as centred now.\n"
               "Press Calibrate for the best results.");
    else
        say(s, "Calibrated. OK keeps the settings.");
}

/* what can be used now: not while calibrating, not without a stick */
static void enable_all(joyapp_t *s) {
    bool p = joy.present, idle = s->step == 0;
    ctl_enable(s->cal, p);
    ctl_enable(s->reset, p && idle);
    ctl_enable(s->detect, idle);
    ctl_enable(s->ok, idle);
    ctl_enable(s->dz, p);
    for (int i = 0; i < JOY_AXES; i++)
        ctl_enable(s->inv[i], p && (joy.axes & (1 << i)));
    s->shown_present = p;
}

static void to_idle(joyapp_t *s) {
    s->step = 0;
    s->range_short = false;
    joy_cal_range(false);
    ctl_set_text(s->cal, "Calibrate");
    enable_all(s);
    idle_message(s);
}

/* Next, from the button or a button on the stick */
static void next_step(joyapp_t *s) {
    if (s->step == 1) {
        if (!joy_cal_center()) {
            say(s, "The stick did not answer. Try again.");
            return;
        }
        s->step = 2;
        joy_cal_range(true);
        say(s, "2 of 2: roll the stick to every edge a few times,\n"
               "then press Next.");
    } else if (s->step == 2) {
        if (joy_cal_done()) {
            to_idle(s);
        } else {
            joy_cal_range(true);
            say(s, "An axis did not move far enough. Keep rolling it\n"
                   "to both ends, then press Next.");
        }
    }
}

static void on_cal(control *c, int ev) {
    joyapp_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    if (s->step == 0) {
        s->step = 1;
        ctl_set_text(s->cal, "Next");
        enable_all(s);
        say(s, "1 of 2: let go of the stick (centred),\n"
               "then press Next or a button on the stick.");
    } else {
        next_step(s);
    }
}

static void on_reset(control *c, int ev) {
    joyapp_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    joy_cal_reset();
    ctl_set_value(s->dz, g_cfg.joy.deadzone);
    for (int i = 0; i < JOY_AXES; i++)
        ctl_set_value(s->inv[i], 0);
    idle_message(s);
}

static void on_detect(control *c, int ev) {
    joyapp_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    if (joy_detect()) {
        char m[64];
        int n = 0;
        for (int i = 0; i < JOY_AXES; i++)
            n += (joy.axes >> i) & 1;
        snprintf(m, sizeof(m), "Found a joystick with %d axes.", n);
        enable_all(s);
        say(s, m);
    } else {
        enable_all(s);
        idle_message(s);
    }
}

static void on_dz(control *c, int ev) {
    joyapp_t *s = c->user;
    if (ev != EV_CHANGE)
        return;
    g_cfg.joy.deadzone = (uint8_t)ctl_value(c);
    char t[12];
    snprintf(t, sizeof(t), "%d%%", g_cfg.joy.deadzone);
    ctl_set_text(s->dz_lbl, t);
}

static void on_inv(control *c, int ev) {
    if (ev != EV_CHANGE)
        return;
    uint8_t bit = (uint8_t)(1 << c->id);
    if (ctl_value(c))
        g_cfg.joy.invert |= bit;
    else
        g_cfg.joy.invert &= (uint8_t)~bit;
}

static void m_close(void) { os_quit_app_by_desc(&joyconf_app); }

static void on_ok(control *c, int ev) {
    (void)c;
    if (ev != EV_CLICK)
        return;
    cfg_save(); /* (false from the setup floppy: it stays until restart) */
    m_close();
}

/* Cancel, Esc, the X and Ctrl+Q: the settings it started with */
static bool on_window_close(window *w) {
    joyapp_t *s = w->userdata;
    joy_cal_range(false);
    if (s)
        g_cfg.joy = s->saved;
    m_close();
    return true;
}

static void on_cancel(control *c, int ev) {
    if (ev == EV_CLICK)
        on_window_close(c->win);
}

static void joyconf_init(void *state) {
    joyapp_t *s = state;
    s->win = app_window(s, "Joystick", 30, 12, 260, 178, DARK_GRAY, 0);
    if (!s->win)
        return;
    window *w = s->win;
    s->saved = g_cfg.joy;
    w->on_close = on_window_close;
    ctl_custom(w, 6, 4, 248, 78, paint_view, NULL, CS_BORDER);

    ctl_label(w, 8, 88, 54, 12, "Dead zone:", 0);
    s->dz = ctl_slider(w, 62, 88, 110, 0, 40, g_cfg.joy.deadzone, on_dz);
    ctl_set_tooltip(s->dz, "The middle of the travel that counts as centred");
    s->dz_lbl = ctl_label(w, 178, 88, 40, 12, "", 0);
    on_dz(s->dz, EV_CHANGE);
    ctl_label(w, 8, 103, 54, 12, "Reverse:", 0);
    for (int i = 0; i < JOY_AXES; i++) {
        char t[2] = {AXIS_NAME[i], 0};
        s->inv[i] = ctl_check(w, 62 + i * 36, 102, 32, t,
                              (g_cfg.joy.invert >> i) & 1, on_inv);
        s->inv[i]->id = i;
    }
    s->msg = ctl_label(w, 8, 119, 246, 20, "", 0);

    s->cal = ctl_button(w, 8, 144, 60, 13, "Calibrate", 0, on_cal);
    ctl_set_tooltip(s->cal, "Two steps: centre, then every edge");
    s->reset = ctl_button(w, 72, 144, 40, 13, "Reset", 0, on_reset);
    ctl_set_tooltip(s->reset, "Back to no calibration");
    s->detect = ctl_button(w, 116, 144, 44, 13, "Detect", 0, on_detect);
    ctl_set_tooltip(s->detect, "Look for the stick again");
    s->ok = ctl_button(w, 168, 144, 40, 13, "OK", 1, on_ok);
    ctl_style_add(s->ok, CS_DEFAULT);
    control *cn = ctl_button(w, 212, 144, 40, 13, "Cancel", 1, on_cancel);
    ctl_style_add(cn, CS_CANCEL);
    enable_all(s);
    idle_message(s);

    menu *f = window_add_menu(w, "Joystick");
    menu_add_item(f, "Close", m_close);
}

/* the stick may appear or go: follow it. A button on it is Next. */
static void joyconf_frame(void *state) {
    joyapp_t *s = state;
    if (!s->win)
        return;
    if (joy.present != s->shown_present) {
        if (!joy.present && s->step)
            to_idle(s);
        enable_all(s);
        idle_message(s);
    }
    if (s->step && joy.pressed)
        next_step(s);
}

app_descriptor joyconf_app = {
    .name = "JOYSTICK",
    .state_size = sizeof(joyapp_t),
    .init = joyconf_init,
    .on_frame = joyconf_frame,
    .single_instance = true,
};
