#include "os/api.h"

#define UPDATE_INTERVAL 2000
#define MEM_LABEL_IDX 1
#define STOR_LABEL_IDX 3

typedef struct {
    window  *win;
    char     time[32];
    char     date[32];
    uint32_t frame_counter;
} clock_state_t;

app_descriptor clock_app;

static void update_label_text(window *win, int idx, char *new_text) {
    if (!win || idx < 0 || idx >= win->widget_count) return;
    win->widgets[idx].as.label.text = new_text;
}

static void write_2digit(char *buf, uint8_t val) {
    buf[0] = '0' + (val / 10);
    buf[1] = '0' + (val % 10);
}

static void write_2digit_year(char *buf, uint32_t val) {
    buf[0] = '0' + (val / 1000);
    buf[1] = '0' + (val % 1000 / 100);
    buf[2] = '0' + (val % 100 / 10);
    buf[3] = '0' + (val % 10);
}

static void format_time(char *out, char *out2) {
    time_full_t t = time_rtc_local();
    write_2digit(out + 0, t.hours);
    out[2] = ':';
    write_2digit(out + 3, t.minutes);
    out[5] = ':';
    write_2digit(out + 6, t.seconds);
    out[8] = '\0';

    write_2digit(out2 + 0, t.day);
    out2[2] = '/';
    write_2digit(out2 + 3, t.month);
    out2[5] = '/';
    write_2digit_year(out2 + 6, t.year);
    out2[10] = '\0';
}

static void clock_refresh(clock_state_t *s) {
	char t[32], d[32];
	format_time(t, d);
	strcpy(s->time, t);
	strcpy(s->date, d);
}

static bool clock_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&clock_app);
    return true;
}

static void on_file_close() {
	clock_close(NULL);
}

static void clock_init(void *state) {
    clock_state_t *s = (clock_state_t *)state;

    const window_spec_t spec = {
        .x             = 20,
        .y             = 20,
        .w             = 100,
        .h             = 60,
        .title         = "Clock",
        .title_color   = WHITE,
        .bar_color     = BLACK,
        .content_color = LIGHT_GRAY,
        .visible       = true,
        .on_close      = clock_close,
        .resizable     = false,
    };
    s->win = wm_register(&spec);
    if (!s->win) return;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);

    window_add_widget(s->win, make_label(15, 6,  s->time,  WHITE, 1));
    window_add_widget(s->win, make_label(8, 16, "----------", WHITE, 1));
    window_add_widget(s->win, make_label(8, 26, s->date, WHITE, 1));

    clock_refresh(s);
}

static void clock_on_frame(void *state) {
    clock_state_t *s = (clock_state_t *)state;
    if (!s->win) return;

    s->frame_counter++;
    if (s->frame_counter < UPDATE_INTERVAL) return;
    s->frame_counter = 0;

    clock_refresh(s);
}

static void clock_destroy(void *state) {
    clock_state_t *s = (clock_state_t *)state;
    wm_unregister(s->win);
    s->win = NULL;
}

app_descriptor clock_app = {
    .name       = "Clock",
    .state_size = sizeof(clock_state_t),
    .init       = clock_init,
    .on_frame   = clock_on_frame,
    .destroy    = clock_destroy,
};
