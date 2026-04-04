#include "os/api.h"

typedef struct {
    window *win_settings;
    window *win_info;
} finder_state_t;

app_descriptor finder_app;

static bool finder_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&finder_app);
    return true;
}

static void on_file_close() {
	finder_close(NULL);
}

static void finder_init(void *state) {
    finder_state_t *s = (finder_state_t *)state;
    app_instance_t *existing[2];
    if (os_find_instances(&finder_app, existing, 2) > 1) {
        os_quit_app(state);
    }

    const window_spec_t settings_spec = {
        .x             = 20,
        .y             = 20,
        .w             = 200,
        .h             = 150,
        .title         = "Settings",
        .title_color   = 15,
        .bar_color     = 12,
        .content_color = 7,
        .visible       = true,
        .on_close      = finder_close,
    };
    s->win_settings = wm_register(&settings_spec);
    if (!s->win_settings) return;

    menu *file_menu = window_add_menu(s->win_settings, "File");
    menu_add_item(file_menu, "Close", on_file_close);
}

static void finder_on_frame(void *state) {
    (void)state;
}

static void finder_destroy(void *state) {
    finder_state_t *s = (finder_state_t *)state;

    wm_unregister(s->win_settings); s->win_settings = NULL;
}

app_descriptor finder_app = {
    .name       = "Finder",
    .state_size = sizeof(finder_state_t),
    .init       = finder_init,
    .on_frame   = finder_on_frame,
    .destroy    = finder_destroy,
};
