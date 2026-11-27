// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* Clock: the time as text, or on a classic face with hands (View), the
 * date and the alarm. Alarm > Set Alarm turns the window into a page with
 * - and + for the hour and the minutes (hold them to keep going). The
 * desktop rings the alarm, whether Clock runs or not. */

#include "extapps/shim.h"

enum { PAGE_TEXT, PAGE_CLASSIC, PAGE_ALARM };

typedef struct {
    window *win;    /* must stay first */
    int page, face; /* face: the page the clock is shown on */
    int dw, dh;     /* the window's size minus its client area */
    /* the text face */
    control *time, *alarm, *date;
    /* the classic face */
    control *dial, *c_alarm, *c_date;
    /* the alarm page */
    control *lbl[2], *val[2], *btn[4], *on, *ok, *cancel;
    int ah, am;
    int held;         /* the - or + held down (0..3), -1 none */
    uint32_t next_at; /* when it steps again */
    int repeats;      /* steps it made while held */
    char last[32];
    char view_label[16];
    char val_text[2][4];
} clock_state_t;

app_descriptor clock_app;

#define W 112 /* the window's width, every page */

// the classic face: sin and cos in 60ths of a turn, x1024 (no FPU)

static const int16_t SIN60[16] = {0,   107, 213, 316, 416, 512,  602,  685,
                                  761, 828, 887, 935, 974, 1002, 1018, 1024};

static int isin60(int i) {
    i = ((i % 60) + 60) % 60;
    if (i <= 15)
        return SIN60[i];
    if (i <= 30)
        return SIN60[30 - i];
    if (i <= 45)
        return -SIN60[i - 30];
    return -SIN60[60 - i];
}
static int icos60(int i) { return isin60(i + 15); }

/* a hand from the middle, at4: where, in 240ths of a turn (smoother) */
static void hand(int cx, int cy, int at4, int len, int thick, uint8_t c) {
    int a = at4 / 4, f = at4 % 4; /* between two 60ths: weighted */
    int sx = (isin60(a) * (4 - f) + isin60(a + 1) * f) / 4;
    int sy = (icos60(a) * (4 - f) + icos60(a + 1) * f) / 4;
    int ex = cx + sx * len / 1024, ey = cy - sy * len / 1024;
    draw_line(cx, cy, ex, ey, c);
    if (thick > 1) {
        draw_line(cx + 1, cy, ex + 1, ey, c);
        draw_line(cx, cy + 1, ex, ey + 1, c);
    }
    if (thick > 2) {
        draw_line(cx - 1, cy, ex - 1, ey, c);
        draw_line(cx, cy - 1, ex, ey - 1, c);
    }
}

static void paint_dial(control *c, int x, int y, int w, int h) {
    (void)c;
    int r = (w < h ? w : h) / 2 - 1;
    int cx = x + w / 2, cy = y + h / 2;
    if (r < 8)
        return;
    for (int dy = -r; dy <= r; dy++) { /* the face, in a black rim */
        int dx = isqrt(r * r - dy * dy);
        fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, BLACK);
        int in = r - 2;
        if (dy > -in && dy < in) {
            int ix = isqrt(in * in - dy * dy);
            fill_rect(cx - ix, cy + dy, 2 * ix + 1, 1, WHITE);
        }
    }
    for (int i = 0; i < 60; i++) { /* the marks: 12, 3, 6, 9 the longest */
        int out = r - 4, len = i % 15 == 0 ? 7 : i % 5 == 0 ? 4 : 0;
        int px = cx + isin60(i) * out / 1024, py = cy - icos60(i) * out / 1024;
        if (!len) { /* (the minutes: dots, on a big face) */
            if (r > 30)
                draw_dot(px, py, DARK_GRAY);
            continue;
        }
        int qx = cx + isin60(i) * (out - len) / 1024;
        int qy = cy - icos60(i) * (out - len) / 1024;
        draw_line(px, py, qx, qy, BLACK);
        if (i % 15 == 0) { /* (twice as thick) */
            int ox = i % 30 == 0, oy = !ox;
            draw_line(px + ox, py + oy, qx + ox, qy + oy, BLACK);
        }
    }
    time_full_t t = time_rtc_local();
    hand(cx, cy, (t.hours % 12) * 20 + t.minutes / 3, r * 5 / 10, 3, BLACK);
    hand(cx, cy, t.minutes * 4 + t.seconds / 15, r * 8 / 10, 2, BLACK);
    hand(cx, cy, t.seconds * 4, r * 9 / 10, 1, RED);
    hand(cx, cy, t.seconds * 4 + 120, r / 6, 1, RED); /* (its tail) */
    fill_rect(cx - 1, cy - 1, 3, 3, BLACK);
    draw_dot(cx, cy, RED);
}

// the text shown

static void alarm_text(char *buf) {
    bool set;
    int ah, am;
    A->time->alarm_get(&set, &ah, &am); /* the desktop rings it */
    if (set)
        sprintf(buf, "Alarm %02d:%02d", ah, am);
    else
        strcpy(buf, "No alarm");
}

static void refresh(clock_state_t *s) {
    time_full_t t = time_rtc_local();
    char buf[32];
    sprintf(buf, "%02d:%02d:%02d", t.hours, t.minutes, t.seconds);
    if (strcmp(buf, s->last) == 0)
        return;
    strcpy(s->last, buf);
    ctl_set_text(s->time, buf);
    sprintf(buf, "%02d/%02d/%04d", t.day, t.month, t.year);
    ctl_set_text(s->date, buf);
    ctl_set_text(s->c_date, buf);
    alarm_text(buf);
    ctl_set_text(s->alarm, buf);
    ctl_set_text(s->c_alarm, buf);
    if (s->page == PAGE_CLASSIC)
        wm_invalidate(s->win); /* (the hands) */
}

// pages: the window takes the size of the one shown

static void show_page(clock_state_t *s, int page) {
    s->page = page;
    control *text[3] = {s->time, s->alarm, s->date};
    control *classic[3] = {s->dial, s->c_alarm, s->c_date};
    for (int i = 0; i < 3; i++) {
        ctl_show(text[i], page == PAGE_TEXT);
        ctl_show(classic[i], page == PAGE_CLASSIC);
    }
    control *alarm[11] = {s->lbl[0], s->lbl[1], s->val[0], s->val[1],
                          s->btn[0], s->btn[1], s->btn[2], s->btn[3],
                          s->on,     s->ok,     s->cancel};
    for (int i = 0; i < 11; i++)
        ctl_show(alarm[i], page == PAGE_ALARM);
    int ch = page == PAGE_TEXT      ? 44
             : page == PAGE_CLASSIC ? W - s->dw + 16
                                    : 70;
    s->win->h = ch + s->dh;
    if (page == PAGE_ALARM)
        ctl_focus(s->ok);
    s->last[0] = 0; /* (all shown again now) */
    refresh(s);
    wm_invalidate(s->win);
}

static void set_face(clock_state_t *s, int face, bool keep) {
    s->face = face;
    strcpy(s->view_label, face == PAGE_TEXT ? "Classic Clock" : "Text Clock");
    if (keep) /* (for the next time) */
        app_prefs_set_int("Clock", "Face", face);
    show_page(s, face);
}

// the alarm page

static void show_values(clock_state_t *s) {
    sprintf(s->val_text[0], "%02d", s->ah);
    sprintf(s->val_text[1], "%02d", s->am);
    ctl_set_text(s->val[0], s->val_text[0]);
    ctl_set_text(s->val[1], s->val_text[1]);
}

/* - or + (0..3: hour -, hour +, minutes -, minutes +): round they go */
static void step(clock_state_t *s, int b) {
    int d = b & 1 ? 1 : -1;
    if (b < 2)
        s->ah = (s->ah + d + 24) % 24;
    else
        s->am = (s->am + d + 60) % 60;
    show_values(s);
}

static void on_step(control *c, int ev) {
    clock_state_t *s = c->user;
    (void)ev;
    if (s->repeats) { /* (held: it has stepped already) */
        s->repeats = 0;
        return;
    }
    step(s, c->id);
}

/* held down: after a moment it keeps going, then faster */
static void hold_repeat(clock_state_t *s) {
    if (!mouse.left || s->page != PAGE_ALARM) {
        s->held = -1;
        s->repeats = 0;
        return;
    }
    int which = -1;
    for (int i = 0; i < 4; i++) {
        control *b = s->btn[i];
        if (mouse.x >= b->ax && mouse.x < b->ax + b->aw && mouse.y >= b->ay &&
            mouse.y < b->ay + b->ah)
            which = i;
    }
    uint32_t now = time_millis();
    if (which != s->held) {
        s->held = which;
        s->next_at = now + 450;
        return;
    }
    if (which >= 0 && (int32_t)(now - s->next_at) >= 0) {
        step(s, which);
        s->repeats++;
        s->next_at = now + (s->repeats < 8 ? 120 : 50);
    }
}

static void open_alarm(clock_state_t *s) {
    bool set;
    A->time->alarm_get(&set, &s->ah, &s->am);
    if (!set) { /* (from now) */
        time_full_t t = time_rtc_local();
        s->ah = t.hours;
        s->am = t.minutes;
    }
    ctl_set_value(s->on, 1);
    show_values(s);
    show_page(s, PAGE_ALARM);
}

static void on_ok(control *c, int ev) {
    clock_state_t *s = c->user;
    (void)ev;
    A->time->alarm_set(ctl_value(s->on) != 0, s->ah, s->am);
    show_page(s, s->face);
}

static void on_cancel(control *c, int ev) {
    clock_state_t *s = c->user;
    (void)ev;
    show_page(s, s->face);
}

// menus

static void on_set_alarm(void) {
    clock_state_t *s = ui_active_state();
    if (s)
        open_alarm(s);
}

static void on_clear_alarm(void) {
    clock_state_t *s = ui_active_state();
    A->time->alarm_set(false, 0, 0);
    if (s)
        s->last[0] = 0;
}

static void on_view(void) {
    clock_state_t *s = ui_active_state();
    if (s)
        set_face(s, s->face == PAGE_TEXT ? PAGE_CLASSIC : PAGE_TEXT, true);
}

static void on_close(void) { os_quit_app_by_desc(&clock_app); }

static void on_about(void) {
    dlg_message(NULL, "About Clock",
                "Clock v4.0\nThe time as text or on a classic face (View).\n"
                "Alarm > Set Alarm: - and + set the time (hold them).\n"
                "(c)2026 @drclcomputers");
}

static void clock_init(void *state) {
    clock_state_t *s = state;
    s->win = app_window(s, "Clock", 20, 20, W, 62, BLACK, 0);
    if (!s->win)
        return;
    int cx, cy, cw, ch;
    win_client_rect(s->win, &cx, &cy, &cw, &ch);
    s->dw = W - cw;
    s->dh = 62 - ch;
    s->held = -1;

    /* the text face */
    s->time = ctl_label(s->win, 0, 4, cw, 10, "", LS_BIG | LS_CENTER);
    s->alarm = ctl_label(s->win, 0, 17, cw, 8, "", LS_CENTER);
    s->date = ctl_label(s->win, 0, 28, cw, 10, "", LS_BIG | LS_CENTER);
    ctl_set_colors(s->alarm, DARK_GRAY, 0xFF);

    /* the classic face, as wide as the window; the date, the alarm under */
    int d = cw - 6;
    s->dial = ctl_custom(s->win, 3, 2, d, d, paint_dial, NULL, 0);
    s->c_date = ctl_label(s->win, 0, d + 3, cw, 8, "", LS_CENTER);
    s->c_alarm = ctl_label(s->win, 0, d + 11, cw, 8, "", LS_CENTER);
    ctl_set_colors(s->c_alarm, DARK_GRAY, 0xFF);

    /* the alarm page: Hour - 07 +, Minutes - 30 +, on or off, OK, Cancel */
    static const char *const NAME[2] = {"Hour", "Minutes"};
    static const char *const TIP[4] = {
        "An hour earlier (hold it to keep going)",
        "An hour later (hold it to keep going)",
        "A minute earlier (hold it to keep going)",
        "A minute later (hold it to keep going)"};
    for (int r = 0; r < 2; r++) {
        int y = 4 + r * 16;
        s->lbl[r] = ctl_label(s->win, 4, y + 3, 40, 8, NAME[r], 0);
        s->btn[r * 2] =
            ctl_button(s->win, cw - 62, y, 16, 13, "-", r * 2, on_step);
        s->val[r] =
            ctl_label(s->win, cw - 44, y + 2, 24, 10, "", LS_BIG | LS_CENTER);
        s->btn[r * 2 + 1] =
            ctl_button(s->win, cw - 20, y, 16, 13, "+", r * 2 + 1, on_step);
    }
    for (int i = 0; i < 4; i++)
        ctl_set_tooltip(s->btn[i], TIP[i]);
    s->on = ctl_check(s->win, 4, 38, cw - 8, "Ring at this time", true, NULL);
    int bw = (cw - 12) / 2;
    s->ok = ctl_button(s->win, 4, 53, bw, 13, "OK", 0, on_ok);
    s->cancel = ctl_button(s->win, 8 + bw, 53, bw, 13, "Cancel", 0, on_cancel);
    ctl_style_add(s->ok, CS_DEFAULT);
    ctl_style_add(s->cancel, CS_CANCEL);

    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "Close", on_close);
    menu_add_separator(f);
    menu_add_item(f, "About Clock", on_about);
    menu *v = window_add_menu(s->win, "View");
    menu_add_item(v, s->view_label, on_view);
    menu *a = window_add_menu(s->win, "Alarm");
    menu_add_item(a, "Set Alarm...", on_set_alarm);
    menu_add_item(a, "Clear Alarm", on_clear_alarm);
    set_face(s,
             app_prefs_get_int("Clock", "Face", PAGE_TEXT) == PAGE_CLASSIC
                 ? PAGE_CLASSIC
                 : PAGE_TEXT,
             false);
}

static void clock_on_frame(void *state) {
    clock_state_t *s = state;
    refresh(s);
    hold_repeat(s);
}

APP_META("Clock", clock_state_t, true, 0, clock_init, clock_on_frame, NULL);
