#include "os/api.h"

static void on_file_new(void)   { /* TODO: open new window */ }
static void on_file_save(void)  { /* TODO: save focused document */ }
static void on_file_close(void) { /* TODO: close focused window */ }
static void on_edit_copy(void)  { }
static void on_edit_paste(void) { }

static window win_settings;
static window win_info;

static void finder_init(void) {
    win_settings = (window){
        .x=20, .y=20, .w=200, .h=150,
        .title         = "Settings",
        .title_color   = 0xFF,
        .bar_color     = 0x08,
        .content_color = 0xEE,
        .visible       = true,
    };
    wm_register(&win_settings);

    menu *file_menu = window_add_menu(&win_settings, "File");
    menu_add_item(file_menu, "New",   on_file_new);
    menu_add_item(file_menu, "Save",  on_file_save);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", on_file_close);

    menu *edit_menu = window_add_menu(&win_settings, "Edit");
    menu_add_item(edit_menu, "Copy",  on_edit_copy);
    menu_add_item(edit_menu, "Paste", on_edit_paste);

    window_add_widget(&win_settings,
        make_label(10, 6, "Username:", 0x00, 2));
    window_add_widget(&win_settings,
        make_textbox(10, 16, 120, 12, 0xFF, 0x00, 0x00));
    window_add_widget(&win_settings,
        make_checkbox(10, 36, "Enable sound", 0xFF, 0x00, 0x00, false, NULL));
    window_add_widget(&win_settings,
        make_button(10, 54, 50, 12, "OK", 0xCC, 0x00, 0x00, NULL));

    // Info window
    win_info = (window){
        .x=60, .y=40, .w=120, .h=80,
        .title         = "Info",
        .title_color   = 0xFF,
        .bar_color     = 0x01,
        .content_color = 0xEE,
        .visible       = true,
    };
    wm_register(&win_info);

    menu *info_file = window_add_menu(&win_info, "File");
    menu_add_item(info_file, "Close", on_file_close);

    window_add_widget(&win_info,
        make_label(10, 6, "My OS v0.1", 0x00, 2));
}

static void finder_on_frame(void) {
    // Per-frame logic for the finder/desktop goes here.
    // Drawing is handled by wm_draw_all() in os_run() — don't call it here.
    // Use this for things like: checking if a file was double-clicked,
    // updating desktop icon state, etc.
}

app_descriptor finder_app = {
    .name     = "Finder",
    .init     = finder_init,
    .on_frame = finder_on_frame,
};
