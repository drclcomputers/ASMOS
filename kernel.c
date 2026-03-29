#include "lib/utils.h"
#include "ui/ui.h"
#include "io/ps2.h"

void kmain(void) {
    ps2_init();

    window my_win = {
        .x=20, .y=30, .w=200, .h=150,
        .title="Settings",
        .title_color=0xFF, .bar_color=0x08, .content_color=0xEE,
        .visible=true
    };
    wm_register(&my_win);

    window_add_widget(&my_win,
        make_checkbox(10, 10, "Enable sound", 0x0F, 0x0F, 0x00, false, NULL));

    window_add_widget(&my_win,
        make_button(10, 30, 60, 12, "OK", 0xCC, 0x00, 0x00, NULL));

    window_add_widget(&my_win,
        make_textbox(10, 50, 120, 12, 0x0F, 0x00, 0x00));

    window my_win2 = {
        .x=60, .y=60, .w=100, .h=50,
        .title="Top",
        .title_color=0xFF, .bar_color=0x08, .content_color=0xEE,
        .visible=true
    };
    wm_register(&my_win2);

    while (1) {
        ps2_update();
        clear_screen(0x0F);
        wm_update_all();
        wm_draw_all();
        draw_cursor(mouse.x, mouse.y);
        blit();
    }
}
