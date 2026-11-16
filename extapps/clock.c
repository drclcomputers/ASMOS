#include "extapps/shim.h"

typedef struct {
    window *win; /* must stay first */
    control *time, *alarm, *date;
    char last[32];
} clock_state_t;

app_descriptor clock_app;

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
    bool set;
    int ah, am;
    A->time->alarm_get(&set, &ah, &am); /* the desktop rings it */
    if (set)
        sprintf(buf, "Alarm %02d:%02d", ah, am);
    else
        strcpy(buf, "No alarm");
    ctl_set_text(s->alarm, buf);
}

static void alarm_entered(void *user, const char *text) {
    clock_state_t *s = user;
    if (!text)
        return;
    int d[4], n = 0;
    for (int i = 0; text[i] && n < 4; i++)
        if (text[i] >= '0' && text[i] <= '9')
            d[n++] = text[i] - '0';
    int h = d[0] * 10 + d[1], m = d[2] * 10 + d[3];
    if (n != 4 || h > 23 || m > 59) {
        dlg_message(s->win, "Set Alarm", "Use HH:MM, e.g. 07:30.");
        return;
    }
    A->time->alarm_set(true, h, m);
    s->last[0] = 0; /* redraw now */
}

static void on_set_alarm(void) {
    clock_state_t *s = ui_active_state();
    if (s)
        dlg_input(s->win, "Set Alarm", "Alarm time (HH:MM):", "07:30", 5, 0,
                  alarm_entered, s);
}

static void on_clear_alarm(void) {
    clock_state_t *s = ui_active_state();
    A->time->alarm_set(false, 0, 0);
    if (s)
        s->last[0] = 0;
}

static void on_close(void) { os_quit_app_by_desc(&clock_app); }

static void on_about(void) {
    dlg_message(NULL, "About Clock",
                "Clock v3.0\nASMOS System App\n(c)2026 @drclcomputers");
}

static void clock_init(void *state) {
    clock_state_t *s = state;
    s->win = app_window(s, "Clock", 20, 20, 104, 62, BLACK, 0);
    if (!s->win)
        return;
    s->time = ctl_label(s->win, 0, 4, 98, 10, "", LS_BIG | LS_CENTER);
    s->alarm = ctl_label(s->win, 0, 17, 98, 8, "", LS_CENTER);
    s->date = ctl_label(s->win, 0, 28, 98, 10, "", LS_BIG | LS_CENTER);
    ctl_set_colors(s->alarm, DARK_GRAY, 0xFF);

    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "Close", on_close);
    menu_add_separator(f);
    menu_add_item(f, "About Clock", on_about);
    menu *a = window_add_menu(s->win, "Alarm");
    menu_add_item(a, "Set Alarm...", on_set_alarm);
    menu_add_item(a, "Clear Alarm", on_clear_alarm);
    refresh(s);
}

static void clock_on_frame(void *state) { refresh(state); }

APP_META("Clock", clock_state_t, true, 0, clock_init, clock_on_frame, NULL);
