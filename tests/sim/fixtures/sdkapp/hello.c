/* Hello - a small ASMOS app, built with the SDK alone (the sdkapp test):
 * a window, an edit box, a button, a menu.
 * asmos.h gives the same functions ASMOS's own apps use (ctl_button,
 * dlg_message, snprintf...); A is the app API for the rest. */
#include "asmos.h"

typedef struct {
    window *win; /* must stay first */
    control *name, *greeting, *count;
    int clicks;
} hello_t;

static void on_greet(control *c, int ev) {
    hello_t *s = c->win->userdata;
    if (ev != EV_CLICK && ev != EV_SUBMIT)
        return;
    const char *who = ctl_text(s->name);
    char msg[64];
    snprintf(msg, sizeof(msg), "Hello, %s!", who[0] ? who : "world");
    ctl_set_text(s->greeting, msg);
    snprintf(msg, sizeof(msg), "Greeted %d time%s", ++s->clicks,
             s->clicks == 1 ? "" : "s");
    ctl_set_text(s->count, msg);
}

static void on_about(void) {
    dlg_message(NULL, "About Hello", "Hello\nBuilt with the ASMOS SDK.");
}

static void hello_init(void *state) {
    hello_t *s = state;
    s->win = app_window(s, "Hello", 60, 50, 200, 96, BLUE, 0);
    if (!s->win)
        return;
    ctl_label(s->win, 8, 8, 40, 12, "Name:", 0);
    s->name = ctl_edit(s->win, 48, 6, 144, 14, "", 32, 0, on_greet);
    ctl_button(s->win, 8, 28, 60, 14, "Greet", 1, on_greet);
    s->greeting = ctl_label(s->win, 8, 50, 184, 12, "", 0);
    s->count = ctl_label(s->win, 8, 64, 184, 12, "", 0);
    menu *m = window_add_menu(s->win, "Hello");
    menu_add_item(m, "About Hello", on_about);
}

APP_META("Hello", hello_t, true, 0, hello_init, NULL, NULL);
