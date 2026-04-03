#include "os/api.h"

typedef struct {
    window *win_settings;
    window *win_info;
} finder_state_t;

app_descriptor finder_app;

static void on_file_new(void)   { /* TODO: open new document window */ }
static void on_file_save(void)  { /* TODO: save focused document    */ }
static void on_file_close(void) { /* TODO: close focused window     */ }
static void on_edit_copy(void)  { /* TODO */ }
static void on_edit_paste(void) { /* TODO */ }

static bool finder_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&finder_app);
    return true;
}

static void finder_init(void *state) {
    finder_state_t *s = (finder_state_t *)state;

    const window_spec_t settings_spec = {
        .x             = 20,
        .y             = 20,
        .w             = 200,
        .h             = 150,
        .title         = "Settings",
        .title_color   = 0xFF,
        .bar_color     = 0x08,
        .content_color = 0xEE,
        .visible       = true,
        .on_close      = finder_close,
    };
    s->win_settings = wm_register(&settings_spec);
    if (!s->win_settings) return;

    menu *file_menu = window_add_menu(s->win_settings, "File");
    menu_add_item(file_menu, "New",   on_file_new);
    menu_add_item(file_menu, "Save",  on_file_save);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", on_file_close);

    menu *edit_menu = window_add_menu(s->win_settings, "Edit");
    menu_add_item(edit_menu, "Copy",  on_edit_copy);
    menu_add_item(edit_menu, "Paste", on_edit_paste);

    window_add_widget(s->win_settings,
        make_label(10, 6, "Username:", 0x00, 2));
    window_add_widget(s->win_settings,
        make_textbox(10, 16, 120, 12, 0xFF, 0x00, 0x00));
    window_add_widget(s->win_settings,
        make_checkbox(10, 36, "Enable sound", 0xFF, 0x00, 0x00, false, NULL));
    window_add_widget(s->win_settings,
        make_button(10, 54, 50, 12, "OK", 0xCC, 0x00, 0x00, NULL));

    const window_spec_t info_spec = {
        .x             = 60,
        .y             = 40,
        .w             = 120,
        .h             = 80,
        .title         = "Info",
        .title_color   = 0xFF,
        .bar_color     = 0x01,
        .content_color = 0xEE,
        .visible       = true,
        .on_close      = finder_close,
    };
    s->win_info = wm_register(&info_spec);
    if (!s->win_info) return;

    menu *info_file = window_add_menu(s->win_info, "File");
    menu_add_item(info_file, "Close", on_file_close);

    window_add_widget(s->win_info,
        make_label(10, 6, "My OS v0.1", 0x00, 2));
}

static void finder_on_frame(void *state) {
    (void)state;
}

static void finder_destroy(void *state) {
    finder_state_t *s = (finder_state_t *)state;

    wm_unregister(s->win_settings); s->win_settings = NULL;
    wm_unregister(s->win_info);     s->win_info     = NULL;
}

app_descriptor finder_app = {
    .name       = "Finder",
    .state_size = sizeof(finder_state_t),
    .init       = finder_init,
    .on_frame   = finder_on_frame,
    .destroy    = finder_destroy,
};
