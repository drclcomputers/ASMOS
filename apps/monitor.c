#include "os/api.h"

typedef struct {
    window *win;
    char memory_str[64];
    char storage_str[64];
    uint32_t last_update_time;
} monitor_state_t;

static void on_file_close(void) {
    // Window will be closed by the app framework
}

static void monitor_init(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;

    s->win = (window *)kmalloc(sizeof(window));
    if (!s->win) return;

    *s->win = (window){
        .x = 20,
        .y = 20,
        .w = 240,
        .h = 140,
        .title         = "Monitor",
        .title_color   = 0xFF,
        .bar_color     = 0x08,
        .content_color = 0xEE,
        .visible       = true,
    };
    wm_register(s->win);

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);

    // Memory label header
    window_add_widget(s->win,
        make_label(10, 8, "Memory:", 0x00, 2));

    // Memory value label
    strcpy(s->memory_str, "Heap: 0 MB / 0 MB");
    window_add_widget(s->win,
        make_label(90, 8, s->memory_str, 0x00, 2));

    // Storage label header
    window_add_widget(s->win,
        make_label(10, 28, "Storage:", 0x00, 2));

    // Storage value label
    strcpy(s->storage_str, "Storage: 0 MB / 0 MB");
    window_add_widget(s->win,
        make_label(90, 28, s->storage_str, 0x00, 2));

    s->last_update_time = 0;
}

static void monitor_on_frame(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;
    if (!s->win) return;

    s->last_update_time++;
    if (s->last_update_time < 2000) return;
    s->last_update_time = 0;

    // Get memory information
    uint32_t heap_used_kb = heap_used();
    uint32_t heap_remaining_kb = heap_remaining();
    uint32_t heap_total_kb = heap_used_kb + heap_remaining_kb;
    uint32_t heap_used_mb = heap_used_kb;
    uint32_t heap_total_mb = heap_total_kb;

    // Build memory string
    char used_str[16] = {0};
    char total_str[16] = {0};
    uint32_to_str(heap_used_mb, used_str);
    uint32_to_str(heap_total_mb, total_str);

    strcpy(s->memory_str, "Heap: ");
    strcat(s->memory_str, used_str);
    strcat(s->memory_str, " B / ");
    strcat(s->memory_str, total_str);
    strcat(s->memory_str, " B");

    // Get storage information
    uint32_t total_bytes = 0;
    uint32_t used_bytes = 0;
    if (fat16_get_usage(&total_bytes, &used_bytes)) {
        uint32_t total_mb = total_bytes;
        uint32_t used_mb = used_bytes;

        // Build storage string
        char used_str_storage[16] = {0};
        char total_str_storage[16] = {0};
        uint32_to_str(used_mb, used_str_storage);
        uint32_to_str(total_mb, total_str_storage);

        strcpy(s->storage_str, "Storage: ");
        strcat(s->storage_str, used_str_storage);
        strcat(s->storage_str, " B / ");
        strcat(s->storage_str, total_str_storage);
        strcat(s->storage_str, " B");
    }
}

static void monitor_destroy(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;

    if (s->win) {
        wm_unregister(s->win);
        kfree(s->win);
        s->win = NULL;
    }
}

app_descriptor monitor_app = {
    .name       = "Monitor",
    .state_size = sizeof(monitor_state_t),
    .init       = monitor_init,
    .on_frame   = monitor_on_frame,
    .destroy    = monitor_destroy,
};
