#include "os/api.h"

#define UPDATE_INTERVAL 60 // update every ~2 seconds

typedef struct {
    window *win;
    char mem_str[64];
    char drive_str[4][64]; // one per possible drive
    int drive_count;
    uint32_t frame_counter;
} monitor_state_t;

app_descriptor monitor_app;

static void fmt_bytes(char *dst, int dst_size, uint32_t used, uint32_t total) {
    char u[12], t[12];
    uint32_to_str(used, u);
    uint32_to_str(total, t);
    int pos = 0;
    for (int i = 0; u[i] && pos < dst_size - 1; i++)
        dst[pos++] = u[i];
    const char *sep = " KB / ";
    for (int i = 0; sep[i] && pos < dst_size - 1; i++)
        dst[pos++] = sep[i];
    for (int i = 0; t[i] && pos < dst_size - 1; i++)
        dst[pos++] = t[i];
    const char *unit = " KB";
    for (int i = 0; unit[i] && pos < dst_size - 1; i++)
        dst[pos++] = unit[i];
    dst[pos] = '\0';
}

static void update_label(window *win, int idx, const char *new_text) {
    if (idx >= 0 && idx < win->widget_count)
        win->widgets[idx].as.label.text = (char *)new_text;
}

static void monitor_refresh(monitor_state_t *s) {
    // Memory
    uint32_t mem_used = heap_used() / 1024;
    uint32_t mem_total = (heap_used() + heap_remaining()) / 1024;
    fmt_bytes(s->mem_str, sizeof(s->mem_str), mem_used, mem_total);
    update_label(s->win, 0, s->mem_str);

    // Drives
    int line = 1;
    for (int d = 0; d < DRIVE_COUNT; d++) {
        if (!fs_drive_mounted(d))
            continue;
        uint32_t tot = 0, used = 0;
        if (fs_get_usage_drive(d, &tot, &used)) {
            used /= 1024;
            tot /= 1024;
            const char *label = fs_drive_label(d);
            const char *vpath = (d == DRIVE_HDA)    ? "/HDA"
                                : (d == DRIVE_HDB)  ? "/HDB"
                                : (d == DRIVE_FDD0) ? "/FDD0"
                                                    : "/FDD1";
            snprintf(s->drive_str[line - 1], sizeof(s->drive_str[0]),
                     "%s (%s): ", label, vpath);
            fmt_bytes(s->drive_str[line - 1] + strlen(s->drive_str[line - 1]),
                      sizeof(s->drive_str[0]) - strlen(s->drive_str[line - 1]) -
                          1,
                      used, tot);
        } else {
            snprintf(s->drive_str[line - 1], sizeof(s->drive_str[0]),
                     "%s: unavailable", fs_drive_label(d));
        }
        update_label(s->win, line, s->drive_str[line - 1]);
        line++;
    }
}

static bool monitor_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&monitor_app);
    return true;
}
static void on_file_close(void) { monitor_close(NULL); }
static void on_about(void) {
    modal_show(MODAL_INFO, "About Monitor",
               "Monitor v1.3\nMulti‑drive disk/memory stats", NULL, NULL);
}

static void monitor_init(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;

    int drives = 0;
    for (int d = 0; d < DRIVE_COUNT; d++)
        if (fs_drive_mounted(d))
            drives++;
    s->drive_count = drives;

    int win_h = 40 + drives * 16;

    const window_spec_t spec = {
        .x = 20,
        .y = 20,
        .w = 200,
        .h = win_h,
        .title = "Monitor",
        .title_color = 15,
        .bar_color = 7,
        .content_color = 10,
        .visible = true,
        .on_close = monitor_close,
        .resizable = false,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "About Monitor", on_about);

    window_add_widget(s->win, make_label(10, 8, s->mem_str, BLACK, 2));

    int y = 26;
    for (int d = 0; d < drives; d++) {
        window_add_widget(s->win, make_label(10, y, s->drive_str[d], BLACK, 2));
        y += 16;
    }

    monitor_refresh(s);
}

static void monitor_on_frame(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;
    if (!s->win)
        return;

    s->frame_counter++;
    if (s->frame_counter < UPDATE_INTERVAL)
        return;
    s->frame_counter = 0;

    monitor_refresh(s);
}

static void monitor_destroy(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;
    wm_unregister(s->win);
    s->win = NULL;
}

app_descriptor monitor_app = {
    .name = "MONITOR",
    .state_size = sizeof(monitor_state_t),
    .init = monitor_init,
    .on_frame = monitor_on_frame,
    .destroy = monitor_destroy,
    .single_instance = true,
};
