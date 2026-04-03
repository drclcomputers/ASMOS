#include "os/api.h"

typedef struct {
    window *win;
} terminal_state_t;

static void terminal_init(void *state) {
    terminal_state_t *s = (terminal_state_t *)state;

    s->win = (window *)kmalloc(sizeof(window));
    if (!s->win) return;

    *s->win = (window){
        .x=20, .y=20, .w=200, .h=150,
        .title         = "Terminal",
        .title_color   = 0x0F,
        .bar_color     = 0x08,
        .content_color = 0x00,
        .visible       = true,
    };
    wm_register(s->win);

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", NULL);

    window_add_widget(s->win,
        make_label(4, 6, ">", 0x0F, 2));
    window_add_widget(s->win,
        make_textbox(12, 2, 170, 12, 0x00, 0x0F, 0x08));
}

static void terminal_on_frame(void *state) {
    (void)state;
    // TODO: read kb input from the textbox widget, run cli_execute_command
}

static void terminal_destroy(void *state) {
    terminal_state_t *s = (terminal_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        kfree(s->win);
        s->win = NULL;
    }
}

app_descriptor terminal_app = {
    .name       = "Terminal",
    .state_size = sizeof(terminal_state_t),
    .init       = terminal_init,
    .on_frame   = terminal_on_frame,
    .destroy    = terminal_destroy,
};
