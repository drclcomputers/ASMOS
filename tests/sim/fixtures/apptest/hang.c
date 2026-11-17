#include "os/appapi.h"
typedef struct { window *win; const os_api_t *api; int frames; } st_t;
void init(const os_api_t *api, void *st) {
    st_t *s = st; s->api = api;
    s->win = api->ui->app_window(s, "Hang", 50, 50, 120, 60, RED, 0);
}
void on_frame(const os_api_t *a, void *st) {
    st_t *s = st; (void)a;
    if (++s->frames == 3) for (;;) {}   /* stuck */
}
void destroy(const os_api_t *a, void *st) { (void)a; (void)st; }
