/*
 * Hello - ASMOS dynamic app (.APP v2).
 * Only os/appapi.h is available: call the kernel through `api->...` and
 * build the UI with the control toolkit in `api->ui` (docs/UI_TOOLKIT.md).
 */
#include "os/appapi.h"

typedef struct {
    window *win; /* must stay first ("state starts with window*") */
    const os_api_t *api;
    control *name, *greeting;
    int clicks;
} app_state_t;

/* Handlers get the control and the event; c->user is the app state. */
static void on_greet(control *c, int ev) {
    app_state_t *s = (app_state_t *)c->user;
    const ui_api_t *ui = s->api->ui;
    char msg[64];
    (void)ev;
    s->api->snprintf(msg, sizeof(msg), "Hello, %s! (%d)",
                     ui->text(s->name)[0] ? ui->text(s->name) : "world",
                     ++s->clicks);
    ui->set_text(s->greeting, msg);
}

/* Menu commands get the app state. */
static void menu_about(void *st) {
    app_state_t *s = (app_state_t *)st;
    s->api->ui->message(s->win, "About", "Hello\nBuilt with api->ui.");
}

static void menu_quit(void *st) {
    app_state_t *s = (app_state_t *)st;
    s->api->quit_self(s);
}

void init(const os_api_t *api, void *st) {
    app_state_t *s = (app_state_t *)st;
    s->api = api;
    if (api->size < sizeof(os_api_t))
        return; /* kernel too old for the toolkit */
    const ui_api_t *ui = api->ui;
    /* client area of a 200x90 window is 198x73 */
    s->win = ui->app_window(s, "Hello", 60, 60, 200, 90, BLUE, AW_RESIZABLE);
    if (!s->win)
        return;
    ui->label(s->win, 6, 8, 40, 10, "Name:", 0);
    s->name = ui->edit(s->win, 40, 6, 152, 12, "", 20, CS_STRETCH_W, 0);
    s->greeting = ui->label(s->win, 6, 26, 186, 10, "Type a name, press Enter.",
                            CS_STRETCH_W);
    control *b = ui->button(s->win, 132, 54, 60, 14, "Greet", 0, on_greet);
    ui->style_add(b, CS_DEFAULT | CS_ANCHOR_RIGHT | CS_ANCHOR_BOTTOM);
    /* CS_DEFAULT: Enter in the window presses it */
    menu *m = ui->add_menu(s->win, "File");
    ui->menu_item(m, "About", menu_about);
    ui->menu_separator(m);
    ui->menu_item(m, "Close", menu_quit);
    ui->focus(s->name);
}

void on_frame(const os_api_t *api, void *st) {
    (void)api;
    (void)st; /* controls are event driven: nothing to poll */
}

void destroy(const os_api_t *api, void *st) {
    (void)api;
    (void)st; /* app_window windows are cleaned up by the OS */
}
