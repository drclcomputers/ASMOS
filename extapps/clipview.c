// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "extapps/shim.h"

typedef struct {
    window *win; /* must stay first */
    control *list, *preview, *copy;
    int seen_count, seen_current;
} cv_state_t;

app_descriptor clipview_app;

static void refill(cv_state_t *s) {
    s->seen_count = clipboard_history_count();
    s->seen_current = g_clip_history_current;
    list_clear(s->list);
    for (int i = s->seen_count - 1; i >= 0; i--) { /* newest first */
        const char *t = clipboard_history_get_text(i);
        char line[64];
        int n = 0;
        for (int k = 0; t && t[k] && n < 60; k++)
            line[n++] = (t[k] == '\n' || t[k] == '\t') ? ' ' : t[k];
        line[n] = 0;
        list_add_data(s->list, line, (uint32_t)i);
    }
    list_set_sel(s->list, 0);
    const char *t = clipboard_history_get_text(s->seen_count - 1);
    ctl_set_text(s->preview, t ? t : "");
    ctl_enable(s->copy, s->seen_count > 0);
}

static void on_list(control *c, int ev) {
    cv_state_t *s = c->user;
    const char *t = clipboard_history_get_text(list_data(c, list_sel(c)));
    if (ev == EV_CHANGE)
        ctl_set_text(s->preview, t ? t : "");
    if (ev == EV_SUBMIT && t)
        clipboard_set_text(t, (int)strlen(t));
}

static void on_copy(control *c, int ev) {
    cv_state_t *s = c->user;
    (void)ev;
    const char *t =
        clipboard_history_get_text(list_data(s->list, list_sel(s->list)));
    if (t)
        clipboard_set_text(t, (int)strlen(t));
}

static void on_close(void) { os_quit_app_by_desc(&clipview_app); }
static void on_about(void) {
    dlg_message(NULL, "About ClipView",
                "Clipboard Viewer v2.0\nShows the text clipboard history.\n"
                "Double-click or Copy puts an entry back.");
}

static void cv_init(void *state) {
    cv_state_t *s = state;
    s->win = app_window(s, "ClipView", 30, 30, 200, 150, DARK_GRAY, 0);
    if (!s->win)
        return;
    s->list = ctl_list(s->win, 4, 4, 190, 54, CS_STRETCH_W, on_list);
    s->preview = ctl_edit(
        s->win, 4, 62, 190, 50, "", 0,
        ES_MULTILINE | ES_READONLY | CS_STRETCH_W | CS_STRETCH_H, NULL);
    s->copy = ctl_button(s->win, 144, 116, 50, 13, "Copy", 1, on_copy);
    ctl_style_add(s->copy, CS_ANCHOR_RIGHT | CS_ANCHOR_BOTTOM);
    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "Close", on_close);
    menu_add_separator(f);
    menu_add_item(f, "About", on_about);
    refill(s);
    ctl_focus(s->list);
}

static void cv_on_frame(void *state) {
    cv_state_t *s = state;
    if (clipboard_history_count() != s->seen_count ||
        g_clip_history_current != s->seen_current)
        refill(s);
}

APP_META("ClipView", cv_state_t, true, 0, cv_init, cv_on_frame, NULL);
