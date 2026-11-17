#include "os/appapi.h"
typedef struct { window *win; const os_api_t *api; int got, after, pref; } st_t;
void init(const os_api_t *api, void *st) {
    st_t *s = st; s->api = api;
    s->win = api->ui->app_window(s, "HeapTest", 40, 40, 120, 60, BLUE, 0);
    int n = 0;
    for (int i = 0; i < 100; i++) if (api->kmalloc(1000)) n++;     /* leak ~64 KB worth */
    s->got = n;
    char *p = api->kmalloc(16);
    if (p) for (int i = 0; i < 64; i++) p[i] = (char)0xEE;           /* smash own heap */
    s->after = api->kmalloc(16) != 0;
    api->kfree(s);                                                    /* must be refused */
    api->prefs_set_int("runs", api->prefs_get_int("runs", 0) + 1);
    s->pref = api->prefs_get_int("runs", -1);
}
void on_frame(const os_api_t *a, void *st) { (void)a; (void)st; }
void destroy(const os_api_t *a, void *st) { (void)a; (void)st; }
