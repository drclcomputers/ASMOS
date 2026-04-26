#include "os/api.h"
#include "os/clipboard.h"

#define CV_DEFAULT_W 180
#define CV_DEFAULT_H 140
#define CV_DEFAULT_X 30
#define CV_DEFAULT_Y 30

#define LINE_H 8
#define CHAR_W 5
#define MARGIN 4

typedef struct {
    window *win;
    int scroll;
    int selected;
} cv_state_t;

app_descriptor clipview_app;

static void cv_about(void) {
    modal_show(MODAL_INFO, "Clipboard Viewer",
               "Clipboard Viewer v1.0\nShows text clipboard history.\nClick a "
               "line to copy it.",
               NULL, NULL);
}

static void cv_draw(window *win, void *ud) {
    cv_state_t *s = (cv_state_t *)ud;
    int wx = win->x + 1, wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2, wh = win->h - 16;

    fill_rect(wx, wy, ww, wh, WHITE);
    int count = clipboard_history_count();
    int y = wy + MARGIN;
    for (int i = s->scroll; i < count && y < wy + wh - 8; i++) {
        const char *t = clipboard_history_get_text(i);
        if (!t)
            continue;
        char line[80];
        int len = (int)strlen(t);
        int max_chars = (ww - 8) / CHAR_W;
        int n = len < max_chars ? len : max_chars;
        memcpy(line, t, n);
        line[n] = '\0';

        if (i == s->selected)
            fill_rect(wx + 2, y - 1, ww - 4, LINE_H + 1, LIGHT_BLUE);

        draw_string(wx + 4, y, line, BLACK, 2);
        y += LINE_H + 2;
    }
}

static void cv_on_frame(void *state) {
    cv_state_t *s = (cv_state_t *)state;
    if (!s->win || !s->win->visible)
        return;

    bool focused = window_is_focused(s->win);

    if (mouse.left_clicked && focused) {
        int wy = s->win->y + MENUBAR_H + 16 + MARGIN;
        int click_y = mouse.y - wy;
        if (click_y >= 0) {
            int idx = s->scroll + click_y / (LINE_H + 2);
            int count = clipboard_history_count();
            if (idx >= 0 && idx < count) {
                s->selected = idx;
                const char *txt = clipboard_history_get_text(idx);
                if (txt) {
                    clipboard_set_text(txt, (int)strlen(txt));
                }
            }
        }
    }

    cv_draw(s->win, s);
}

static void cv_init(void *state) {
    cv_state_t *s = (cv_state_t *)state;
    const window_spec_t spec = {
        .x = CV_DEFAULT_X,
        .y = CV_DEFAULT_Y,
        .w = CV_DEFAULT_W,
        .h = CV_DEFAULT_H,
        .min_w = 80,
        .min_h = 60,
        .resizable = true,
        .title = "Clipboard",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = WHITE,
        .visible = true,
        .on_close = NULL,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;
    s->win->on_draw = cv_draw;
    s->win->on_draw_userdata = s;
    s->scroll = 0;
    s->selected = -1;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "About", cv_about);
}

static void cv_destroy(void *state) {
    cv_state_t *s = (cv_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor clipview_app = {
    .name = "CLIPVIEW",
    .state_size = sizeof(cv_state_t),
    .init = cv_init,
    .on_frame = cv_on_frame,
    .destroy = cv_destroy,
    .single_instance = true,
};
