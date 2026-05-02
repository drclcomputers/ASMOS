#include "os/api.h"

#define LABEL_TIME_IDX 0
#define LABEL_SEP_IDX 1
#define LABEL_DATE_IDX 2

#define MODE_NORMAL 0
#define MODE_ALARM 1
#define MODE_INPUT 2

typedef struct {
    window *win;
    char time_str[32];
    char date_str[32];

    int mode;

    bool alarm_set;
    uint8_t alarm_h, alarm_m;

    char input_buf[6];
    int input_len;
    char alarm_label[32];
} clock_state_t;

app_descriptor clock_app;

static void update_label(window *win, int idx, char *text) {
    if (!win || idx < 0 || idx >= win->widget_count)
        return;
    win->widgets[idx].as.label.text = text;
}

static void clock_refresh(clock_state_t *s) {
    time_full_t t = time_rtc_local();
    sprintf(s->time_str, "%02d:%02d:%02d", t.hours, t.minutes, t.seconds);
    sprintf(s->date_str, "%02d/%02d/%04d", t.day, t.month, t.year);
    update_label(s->win, LABEL_TIME_IDX, s->time_str);
    update_label(s->win, LABEL_DATE_IDX, s->date_str);
}

static bool clock_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&clock_app);
    return true;
}

static void on_file_close(void) { clock_close(NULL); }

static void on_about(void) {
    modal_show(MODAL_INFO, "About Clock", "Clock v2.0\nASMOS System App", NULL,
               NULL);
}

static void on_set_alarm(void) {
    clock_state_t *s = NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (a->running && a->desc == &clock_app) {
            s = a->state;
            break;
        }
    }
    if (!s)
        return;
    s->input_buf[0] = '\0';
    s->input_len = 0;
    s->mode = MODE_INPUT;
}

static void on_clear_alarm(void) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (a->running && a->desc == &clock_app) {
            clock_state_t *s = a->state;
            s->alarm_set = false;
            s->alarm_label[0] = '\0';
            update_label(s->win, LABEL_SEP_IDX, "----------");
            return;
        }
    }
}

#define CHAR_W 5

static void clock_draw_input(clock_state_t *s) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + TITLEBAR_H;
    int ww = s->win->w - 2;
    int wh = s->win->h - TITLEBAR_H;

    int bx = wx + 4, by = wy + wh / 2 - 14, bw = ww - 8, bh = 28;
    fill_rect(bx, by, bw, bh, LIGHT_GRAY);
    draw_rect(bx, by, bw, bh, BLACK);
    draw_string(bx + 3, by + 3, "Alarm HHMM", BLACK, 2);
    fill_rect(bx + 3, by + 12, bw - 6, 10, WHITE);
    draw_rect(bx + 3, by + 12, bw - 6, 10, BLACK);
    draw_string(bx + 5, by + 13, s->input_buf, BLACK, 2);

    extern volatile uint32_t pit_ticks;
    if ((pit_ticks / 50) % 2 == 0) {
        int cx = bx + 5 + s->input_len * CHAR_W;
        draw_string(cx, by + 13, "|", BLACK, 2);
    }
}

static void clock_on_draw(window *win, void *ud) {
    clock_state_t *s = (clock_state_t *)ud;
    if (!s)
        return;
    if (s->mode == MODE_INPUT)
        clock_draw_input(s);
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
    s->win->anim_no_of_frames = 10;
    if (!s->win)
        return;
    s->win->on_draw = clock_on_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "About Clock", on_about);

    menu *alarm_menu = window_add_menu(s->win, "Alarm");
    menu_add_item(alarm_menu, "Set Alarm", on_set_alarm);
    menu_add_item(alarm_menu, "Clear Alarm", on_clear_alarm);

    window_add_widget(s->win, make_label(15, 6, s->time_str, WHITE, 1));
    window_add_widget(s->win, make_label(8, 16, "----------", WHITE, 1));
    window_add_widget(s->win, make_label(8, 26, s->date_str, WHITE, 1));

    clock_refresh(s);
}

static void clock_on_frame(void *state) {
    clock_state_t *s = (clock_state_t *)state;
    if (!s->win)
        return;

    clock_refresh(s);

    if (s->mode == MODE_INPUT) {
        if (kb.key_pressed) {
            if (kb.last_scancode == ESC) {
                s->mode = MODE_NORMAL;
            } else if (kb.last_scancode == ENTER) {
                uint8_t h = 0, m = 0;
                bool valid = false;
                if (s->input_len == 4) {
                    h = (s->input_buf[0] - '0') * 10 + (s->input_buf[1] - '0');
                    m = (s->input_buf[2] - '0') * 10 + (s->input_buf[3] - '0');
                    valid = (h < 24 && m < 60);
                }
                if (valid) {
                    g_alarm.hour = h;
                    g_alarm.minute = m;
                    g_alarm.set = true;
                    g_alarm.fired = false;
                    s->alarm_h = h;
                    s->alarm_m = m;
                    s->alarm_set = true;
                    sprintf(s->alarm_label, "  ~%02d:%02d", h, m);
                    update_label(s->win, LABEL_SEP_IDX, s->alarm_label);
                    s->mode = MODE_NORMAL;
                } else {
                    modal_show(MODAL_ERROR, "Invalid", "Enter HHMM (e.g. 0730)",
                               NULL, NULL);
                    s->mode = MODE_NORMAL;
                }
            } else if (kb.last_scancode == BACKSPACE) {
                if (s->input_len > 0)
                    s->input_buf[--s->input_len] = '\0';
            } else if (kb.last_char >= '0' && kb.last_char <= '9' &&
                       s->input_len < 4) {
                s->input_buf[s->input_len++] = kb.last_char;
                s->input_buf[s->input_len] = '\0';
            }
        }
        clock_draw_input(s);
    }
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
    .single_instance = true,
};
