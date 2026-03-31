#include "os/api.h"

static window win_terminal;
static widget input_box;

static void on_close(void) { /* TODO */ }

static void terminal_init(void) {
    win_terminal = (window){
        .x=20, .y=20, .w=200, .h=150,
        .title         = "Terminal",
        .title_color   = 0x0F,
        .bar_color     = 0x08,
        .content_color = 0x00,
        .visible       = true,
    };
    wm_register(&win_terminal);

    menu *file_menu = window_add_menu(&win_terminal, "File");
    menu_add_item(file_menu, "Close", on_close);

    window_add_widget(&win_terminal,
        make_label(4, 6, ">", 0x0F, 2));
    window_add_widget(&win_terminal,
        make_textbox(12, 2, 170, 12, 0x00, 0x0F, 0x08));
}

static void terminal_on_frame(void) {
    // Future: read kb.last_char, check for Enter, run commands
}

app_descriptor terminal_app = {
    .name     = "Terminal",
    .init     = terminal_init,
    .on_frame = terminal_on_frame,
};
