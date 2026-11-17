#include "os/appapi.h"
typedef struct { window *win; const os_api_t *api; control *view; int tick, painted; } st_t;
static void paint(control *c, int x, int y, int w, int h) {
    st_t *s = c->user; s->painted++;
    s->api->fill_rect(x, y, w, h, (uint8_t)(1 + (s->tick / 20) % 14));
    char b[16]; s->api->snprintf(b, 16, "%d", s->tick / 20);
    s->api->draw_string(x + 4, y + 4, b, 15, 4);
}
void init(const os_api_t *api, void *p) {
    st_t *s = p; s->api = api;
    s->win = api->ui->app_window(s, "Lazy", 50, 50, 120, 70, 1, 0);
    s->view = api->ui->custom(s->win, 0, 0, 118, 53, paint, 0, 0);
    api->set_lazy(s->win, true);
}
void on_frame(const os_api_t *api, void *p) {
    st_t *s = p;
    if (++s->tick % 20 == 0) api->invalidate(s->win);
}
