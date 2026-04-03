#include "os/api.h"

#define TEXTBOX_IDX 1

typedef struct {
    window *win;
    bool    waiting_for_enter;
} terminal_state_t;

app_descriptor terminal_app;

static bool terminal_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&terminal_app);
    return true;
}

static void on_file_close() {
	terminal_close(NULL);
}

static void terminal_init(void *state) {
    terminal_state_t *s = (terminal_state_t *)state;

    const window_spec_t spec = {
        .x             = 20,
        .y             = 20,
        .w             = 200,
        .h             = 150,
        .title         = "Terminal",
        .title_color   = 0x0F,
        .bar_color     = 0x08,
        .content_color = 0x00,
        .visible       = true,
        .on_close      = terminal_close,
    };
    s->win = wm_register(&spec);
    if (!s->win) return;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);

    window_add_widget(s->win, make_label(4, 6, ">", WHITE, 2));
    window_add_widget(s->win, make_textbox(12, 2, 170, 42, BLACK, WHITE, LIGHT_GRAY));
    window_add_widget(s->win, make_textbox(12, 52, 170, 42, BLACK, WHITE, LIGHT_GRAY));
}

static void terminal_on_frame(void *state) {
    terminal_state_t *s = (terminal_state_t *)state;
    if (!s->win) return;

    if (s->win->widget_count <= TEXTBOX_IDX) return;

    widget         *tb     = &s->win->widgets[TEXTBOX_IDX];
    widget         *tbout  = &s->win->widgets[TEXTBOX_IDX+1];
    widget_textbox *wtb    = &tb->as.textbox;
    widget_textbox *wtbout = &tbout->as.textbox;

    if (!wtb->focused) return;
    if (wtbout->focused) return;
    if (!wtbout->focused) return;
    if (!kb.key_pressed || kb.last_scancode != ENTER) return;

    if (wtb->len > 0) {
    	char outbuf[1024];
        cli_execute_command(wtb->buf, outbuf, 1024);

        wtbout->len = strlen(outbuf);
        strcpy(wtbout->buf, outbuf);
    }
}

static void terminal_destroy(void *state) {
    terminal_state_t *s = (terminal_state_t *)state;
    wm_unregister(s->win);
    s->win = NULL;
}

app_descriptor terminal_app = {
    .name       = "Terminal",
    .state_size = sizeof(terminal_state_t),
    .init       = terminal_init,
    .on_frame   = terminal_on_frame,
    .destroy    = terminal_destroy,
};
