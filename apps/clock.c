#include "os/api.h"

#define UPDATE_INTERVAL 60
#define MEM_LABEL_IDX 1
#define STOR_LABEL_IDX 3

typedef struct {
    window *win;
    char time[32];
    char date[32];
    uint32_t frame_counter;
} clock_state_t;

app_descriptor clock_app;

static void update_label_text(window *win, int idx, char *new_text) {
    if (!win || idx < 0 || idx >= win->widget_count)
        return;
    win->widgets[idx].as.label.text = new_text;
}

static void clock_refresh(clock_state_t *s) {
    time_full_t t = time_rtc_local();
    sprintf(s->time, "%02d:%02d:%02d", t.hours, t.minutes, t.seconds);
    sprintf(s->date, "%02d/%02d/%04d", t.day, t.month, t.year);

    update_label_text(s->win, 0, s->time);
    update_label_text(s->win, 2, s->date);
}

static bool clock_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&clock_app);
    return true;
}

static void on_file_close(void) { clock_close(NULL); }

static void on_about(void) {
    modal_show(MODAL_INFO, "About Clock",
               "Clock v1.0\nASMOS System App\nAuthor: You", NULL, NULL);
}

static void clock_init(void *state) {
    clock_state_t *s = (clock_state_t *)state;

    const window_spec_t spec = {
        .x = 20,
        .y = 20,
        .w = 100,
        .h = 60,
        .title = "Clock",
        .title_color = WHITE,
        .bar_color = BLACK,
        .content_color = LIGHT_GRAY,
        .visible = true,
        .on_close = clock_close,
        .resizable = false,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "About Clock", on_about);

    window_add_widget(s->win, make_label(15, 6, s->time, WHITE, 1));
    window_add_widget(s->win, make_label(8, 16, "----------", WHITE, 1));
    window_add_widget(s->win, make_label(8, 26, s->date, WHITE, 1));

    clock_refresh(s);
}

static void clock_on_frame(void *state) {
    clock_state_t *s = (clock_state_t *)state;
    if (!s->win)
        return;

    clock_refresh(s);
}

static void clock_destroy(void *state) {
    clock_state_t *s = (clock_state_t *)state;
    wm_unregister(s->win);
    s->win = NULL;
}

app_descriptor clock_app = {
    .name = "CLOCK",
    .state_size = sizeof(clock_state_t),
    .init = clock_init,
    .on_frame = clock_on_frame,
    .destroy = clock_destroy,
};
