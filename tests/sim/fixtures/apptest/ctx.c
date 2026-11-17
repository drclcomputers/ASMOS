#include "os/appapi.h"
typedef struct { window *win; const os_api_t *api; control *view, *tree, *btn; int picked; } st_t;
static void pick_a(void *p) { ((st_t *)p)->picked = 1; }
static void pick_b(void *p) { ((st_t *)p)->picked = 2; }
static void on_view(control *c, int ev) {
    st_t *s = (st_t *)c->user; const ui_api_t *ui = s->api->ui;
    if (ev != EV_MENU) return;
    menu *m = ui->popup_menu();
    ui->menu_item(m, "Alpha", pick_a);
    ui->menu_item(m, "Beta", pick_b);
    ui->menu_separator(m);
    ui->menu_item(m, "Gamma", pick_a);
    ui->menu_enable(m, false);
    ui->popup_show_for(c);
}
static void on_tree(control *c, int ev) {
    st_t *s = (st_t *)c->user; const ui_api_t *ui = s->api->ui;
    if (ev == EV_EXPAND) { int n = ui->tree_event_node(c); ui->tree_add(c, n, "KID", 7, false); }
}
static void paint(control *c, int x, int y, int w, int h) { (void)c; (void)x; (void)y; (void)w; (void)h; }
void init(const os_api_t *api, void *st) {
    st_t *s = st; s->api = api;
    const ui_api_t *ui = api->ui;
    if (ui->size < sizeof(ui_api_t)) return;
    s->win = ui->app_window(s, "CtxTest", 60, 60, 200, 100, BLUE, 0);
    s->view = ui->custom(s->win, 4, 4, 100, 60, paint, on_view, CS_BORDER);
    s->tree = ui->tree(s->win, 108, 4, 86, 60, TS_FOLDERS, on_tree);
    int r = ui->tree_add(s->tree, -1, "ROOT", 1, true);
    ui->tree_expand(s->tree, r, true);
    s->btn = ui->button(s->win, 4, 68, 60, 14, "Tip", 0, 0);
    ui->set_tooltip(s->btn, "Button tip");
}
void on_frame(const os_api_t *a, void *st) { (void)a; (void)st; }
void destroy(const os_api_t *a, void *st) { (void)a; (void)st; }
