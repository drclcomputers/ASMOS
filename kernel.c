#include "lib/utils.h"
#include "ui/ui.h"
#include "io/ps2.h"


static void on_file_new(void)   { /* open a new window etc */ }
static void on_file_save(void)  { /* save logic here       */ }
static void on_file_close(void) { /* close focused window  */ }

static void on_edit_copy(void)  { }
static void on_edit_paste(void) { }

void createterminalapp() {
	window terminal = {
        .x=20, .y=20, .w=200, .h=150,
        .title        = "Terminal",
        .title_color  = 0x08,
        .bar_color    = 0x10,
        .content_color= 0x00,
        .visible      = true,
        .minimized    = false,
        .on_close     = NULL,
        .on_minimize  = NULL,
    };
    wm_register(&terminal);

    menu *file_menu = window_add_menu(&terminal, "File");
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", on_file_close);

    window_add_widget(&terminal,
        make_label(10, 6, ">", 0x0F, 2));
    window_add_widget(&terminal,
        make_textbox(17, 2, 170, 12, 0x0F, 0x00, 0x00));
}

void kmain(void) {
    ps2_init();
    menubar_init();

    createterminalapp();

    window win_settings = {
        .x=20, .y=20, .w=200, .h=150,
        .title        = "Settings",
        .title_color  = 0xFF,
        .bar_color    = 0x08,
        .content_color= 0xEE,
        .visible      = true,
        .minimized    = false,
        .on_close     = NULL,
        .on_minimize  = NULL,
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
        make_textbox(10, 16, 120, 12, 0x0F, 0x00, 0x00));
    window_add_widget(&win_settings,
        make_checkbox(10, 36, "Enable sound", 0x0E, 0x00, 0x00, false, NULL));
    window_add_widget(&win_settings,
        make_button(10, 54, 50, 12, "OK", 0x0C, 0x00, 0x00, NULL));

    window win_info = {
        .x=60, .y=40, .w=120, .h=80,
        .title        = "Info",
        .title_color  = 0xFF,
        .bar_color    = 0x01,
        .content_color= 0xEE,
        .visible      = true,
        .minimized    = false,
        .on_close     = NULL,
        .on_minimize  = NULL,
    };
    wm_register(&win_info);

    menu *info_file = window_add_menu(&win_info, "File");
    menu_add_item(info_file, "Close", on_file_close);

    window_add_widget(&win_info,
        make_label(10, 6, "My OS v0.1", 0x00, 2));

    while (1) {
        ps2_update();

        clear_screen(0xD0);

        wm_sync_menubar(&g_menubar);
        menubar_layout(&g_menubar);
        menubar_update(&g_menubar);
        wm_update_all();

        wm_draw_all();
        menubar_draw(&g_menubar);

        draw_cursor(mouse.x, mouse.y);
        blit();
    }
}
