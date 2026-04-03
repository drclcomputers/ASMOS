#include "os/api.h"

#define UPDATE_INTERVAL 2000
#define MEM_LABEL_IDX 1
#define STOR_LABEL_IDX 3

typedef struct {
    window  *win;
    char     memory_str[32];
    char     storage_str[32];
    uint32_t frame_counter;
} monitor_state_t;

app_descriptor monitor_app;

static void fmt_bytes(char *dst, int dst_size, uint32_t used, uint32_t total) {
    char u[12], t[12];
    uint32_to_str(used,  u);
    uint32_to_str(total, t);

    int pos = 0;
    for (int i = 0; u[i] && pos < dst_size - 1; i++) dst[pos++] = u[i];
    const char *sep = "B / ";
    for (int i = 0; sep[i] && pos < dst_size - 1; i++) dst[pos++] = sep[i];
    for (int i = 0; t[i] && pos < dst_size - 1; i++) dst[pos++] = t[i];
    const char *unit = "B";
    for (int i = 0; unit[i] && pos < dst_size - 1; i++) dst[pos++] = unit[i];
    dst[pos] = '\0';
}

static void update_label_text(window *win, int idx, char *new_text) {
    if (!win || idx < 0 || idx >= win->widget_count) return;
    win->widgets[idx].as.label.text = new_text;
}

static void monitor_refresh(monitor_state_t *s) {
    uint32_t used  = heap_used();
    uint32_t total = heap_used() + heap_remaining();
    fmt_bytes(s->memory_str, (int)sizeof(s->memory_str), used, total);
    update_label_text(s->win, MEM_LABEL_IDX, s->memory_str);

    uint32_t stor_total = 0, stor_used = 0;
    if (fat16_get_usage(&stor_total, &stor_used)) {
        fmt_bytes(s->storage_str, (int)sizeof(s->storage_str), stor_used, stor_total);
    } else {
        strcpy(s->storage_str, "unavailable");
    }
    update_label_text(s->win, STOR_LABEL_IDX, s->storage_str);
}

static bool monitor_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&monitor_app);
    return true;
}

static void on_file_close() {
	monitor_close(NULL);
}

static void monitor_init(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;

    strcpy(s->memory_str,  "...");
    strcpy(s->storage_str, "...");

    const window_spec_t spec = {
        .x             = 20,
        .y             = 20,
        .w             = 160,
        .h             = 80,
        .title         = "Monitor",
        .title_color   = 15,
        .bar_color     = 7,
        .content_color = 10,
        .visible       = true,
        .on_close      = monitor_close,
    };
    s->win = wm_register(&spec);
    if (!s->win) return;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);

    window_add_widget(s->win, make_label(10, 8,  "Memory:",  0, 2));
    window_add_widget(s->win, make_label(50, 8,  s->memory_str,  0, 2));
    window_add_widget(s->win, make_label(10, 28, "Storage:", 0, 2));
    window_add_widget(s->win, make_label(50, 28, s->storage_str, 0, 2));

    monitor_refresh(s);
}

static void monitor_on_frame(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;
    if (!s->win) return;

    s->frame_counter++;
    if (s->frame_counter < UPDATE_INTERVAL) return;
    s->frame_counter = 0;

    monitor_refresh(s);
}

static void monitor_destroy(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;
    wm_unregister(s->win);
    s->win = NULL;
}

app_descriptor monitor_app = {
    .name       = "Monitor",
    .state_size = sizeof(monitor_state_t),
    .init       = monitor_init,
    .on_frame   = monitor_on_frame,
    .destroy    = monitor_destroy,
};
