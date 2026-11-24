// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/lpt.h"
#include "os/api.h"

#define TEXT_MAX (256 * 1024)

typedef struct {
    window *win; /* must stay first */
    control *edit, *status;
    bool dirty;
    char path[FS_PATH_SZ];
    char title[48];
    char find[64];
    int last_line, last_col;
    bool last_dirty;
} teditor_state_t;

app_descriptor teditor_app;

static void set_title(teditor_state_t *s) {
    const char *base = strrchr(s->path, '/');
    base = base ? base + 1 : (s->path[0] ? s->path : "Untitled");
    snprintf(s->title, sizeof(s->title), "%s%s - TEditor", s->dirty ? "*" : "",
             base);
    s->win->title = s->title;
}

static void update_status(teditor_state_t *s, bool force) {
    int line = edit_line_of(s->edit, s->edit->cursor) + 1;
    int col = edit_col_of(s->edit, s->edit->cursor) + 1;
    if (!force && line == s->last_line && col == s->last_col &&
        s->dirty == s->last_dirty)
        return;
    s->last_line = line;
    s->last_col = col;
    s->last_dirty = s->dirty;
    char b[64];
    snprintf(b, sizeof(b), "%d lines  %s\tLn %d  Col %d",
             edit_line_count(s->edit), s->dirty ? "modified" : "", line, col);
    ctl_set_text(s->status, b);
    set_title(s);
}

static bool load(teditor_state_t *s, const char *path) {
    fat_file_t f;
    if (!fs_open(path, &f))
        return false;
    uint32_t n = f.entry.file_size > TEXT_MAX ? TEXT_MAX : f.entry.file_size;
    char *buf = (char *)kmalloc(n + 1);
    if (!buf) {
        fs_close(&f);
        return false;
    }
    int got = fs_read(&f, buf, (int)n);
    fs_close(&f);
    int k = 0;
    for (int i = 0; i < got; i++)
        if (buf[i] != '\r') /* DOS line endings */
            buf[k++] = buf[i];
    buf[k] = 0;
    ctl_set_text(s->edit, buf);
    kfree(buf);
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->dirty = false;
    update_status(s, true);
    return true;
}

static bool save(teditor_state_t *s, const char *path) {
    dir_entry_t de;
    if (fs_find(path, &de))
        fs_delete(path);
    fat_file_t f;
    if (!fs_create(path, &f))
        return false;
    int n = fs_write(&f, ctl_text(s->edit), s->edit->text_len);
    fs_close(&f);
    if (n != s->edit->text_len)
        return false;
    if (path != s->path)
        strncpy(s->path, path, sizeof(s->path) - 1);
    s->dirty = false;
    update_status(s, true);
    return true;
}

static void on_edit(control *c, int ev) {
    teditor_state_t *s = c->user;
    if (ev == EV_CHANGE)
        s->dirty = true;
}

// file commands

static void opened(void *user, const char *path) {
    teditor_state_t *s = user;
    if (path && !load(s, path))
        dlg_message(s->win, "Open", "Could not open the file.");
}

static void saved_as(void *user, const char *path) {
    teditor_state_t *s = user;
    if (path && !save(s, path))
        dlg_message(s->win, "Save", "Could not save (disk full?).");
}

static void dir_of(const char *path, char *out, int n) {
    strncpy(out, path[0] ? path : "/", n - 1);
    out[n - 1] = 0;
    char *sl = strrchr(out, '/');
    if (sl && sl != out)
        *sl = 0;
    else
        strcpy(out, "/");
}

static void do_save_as(teditor_state_t *s) {
    char dir[FS_PATH_SZ];
    dir_of(s->path, dir, sizeof(dir));
    const char *base = strrchr(s->path, '/');
    dlg_save_file(s->win, "Save As", dir, base ? base + 1 : "UNTITLED.TXT",
                  "TXT", saved_as, s);
}

static void do_save(teditor_state_t *s) {
    if (!s->path[0])
        do_save_as(s);
    else if (!save(s, s->path))
        dlg_message(s->win, "Save", "Could not save (disk full?).");
}

static void do_open(teditor_state_t *s) {
    char dir[FS_PATH_SZ];
    dir_of(s->path, dir, sizeof(dir));
    dlg_open_file(s->win, "Open", dir, NULL, opened, s);
}

static void found(void *user, const char *text) {
    teditor_state_t *s = user;
    if (!text || !text[0])
        return;
    strncpy(s->find, text, sizeof(s->find) - 1);
    if (!edit_find(s->edit, s->find, false))
        dlg_message(s->win, "Find", "Not found.");
    ctl_focus(s->edit);
}

static void goto_line(void *user, const char *text) {
    teditor_state_t *s = user;
    if (!text)
        return;
    int n = str_to_int(text);
    if (n > 0)
        edit_goto(s->edit, n - 1, 0);
    ctl_focus(s->edit);
}

static void do_find(teditor_state_t *s) {
    dlg_input(s->win, "Find", "Find text:", s->find, 63, 0, found, s);
}

static void do_find_next(teditor_state_t *s) {
    if (!s->find[0])
        do_find(s);
    else if (!edit_find(s->edit, s->find, false))
        dlg_message(s->win, "Find", "Not found.");
}

static void do_goto(teditor_state_t *s) {
    dlg_input(s->win, "Go to Line", "Line number:", "1", 6, ES_NUMBER,
              goto_line, s);
}

static void do_print(teditor_state_t *s) {
    const char *base = strrchr(s->path, '/');
    base = base ? base + 1 : (s->path[0] ? s->path : "Untitled");
    print_doc_t doc = {PRINT_DOC_TEXT,
                       base,
                       ctl_text(s->edit),
                       (uint32_t)s->edit->text_len,
                       NULL,
                       0,
                       0,
                       0};
    dlg_print(s->win, &doc);
}

/* closing with unsaved changes asks first */
static void confirm_discard(void *user, bool yes) {
    teditor_state_t *s = user;
    if (yes) {
        s->dirty = false;
        os_close_own_instance(s->win);
    }
}

static bool on_close(window *w) {
    teditor_state_t *s = w->userdata;
    if (!s->dirty)
        return os_close_own_instance(w);
    dlg_confirm(w, "TEditor", "Discard unsaved changes?", confirm_discard, s);
    return true;
}

// menus (act on the focused TEditor)
#define ACTION(name, body)                                                     \
    static void name(void) {                                                   \
        teditor_state_t *s = ui_active_state();                                \
        if (s)                                                                 \
            body;                                                              \
    }
static void menu_new(void) { os_launch_app(&teditor_app); }
ACTION(menu_open, do_open(s))
ACTION(menu_save, do_save(s))
ACTION(menu_save_as, do_save_as(s))
ACTION(menu_close, on_close(s->win))
ACTION(menu_print, do_print(s))
ACTION(menu_cut, edit_cut(s->edit))
ACTION(menu_copy, edit_copy(s->edit))
ACTION(menu_paste, edit_paste(s->edit))
ACTION(menu_select_all, edit_select_all(s->edit))
ACTION(menu_find, do_find(s))
ACTION(menu_find_next, do_find_next(s))
ACTION(menu_goto, do_goto(s))
static void menu_about(void) {
    dlg_message(NULL, "About TEditor",
                "TEditor v3.0\nCtrl+N/O/S new/open/save, Ctrl+F find,\n"
                "F3 find next, Ctrl+G go to line.\n(c)2026 @drclcomputers");
}

static void teditor_init(void *state) {
    teditor_state_t *s = state;
    s->win = app_window(s, "TEditor", 30, 20, 260, 176, BLUE, AW_RESIZABLE);
    if (!s->win)
        return;
    s->win->on_close = on_close;
    s->edit = ctl_edit(s->win, 2, 2, 254, 144, "", TEXT_MAX,
                       ES_MULTILINE | CS_STRETCH_W | CS_STRETCH_H, on_edit);
    s->status = ctl_status(s->win, "");
    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "New", menu_new);
    menu_add_item(f, "Open...", menu_open);
    menu_add_item(f, "Save", menu_save);
    menu_add_item(f, "Save As...", menu_save_as);
    menu_add_item(f, "Print...", menu_print);
    menu_add_separator(f);
    menu_add_item(f, "Close", menu_close);
    menu_add_item(f, "About", menu_about);
    menu *e = window_add_menu(s->win, "Edit");
    menu_add_item(e, "Cut", menu_cut);
    menu_add_item(e, "Copy", menu_copy);
    menu_add_item(e, "Paste", menu_paste);
    menu_add_item(e, "Select All", menu_select_all);
    menu_add_separator(e);
    menu_add_item(e, "Find...", menu_find);
    menu_add_item(e, "Find Next", menu_find_next);
    menu_add_item(e, "Go to Line...", menu_goto);
    ctl_focus(s->edit);
    update_status(s, true);
}

static void teditor_on_frame(void *state) {
    teditor_state_t *s = state;
    update_status(s, false);
    /* shortcuts: the edit passes Ctrl+letter (except A/C/X/V) on to us */
    if (!kb.key_pressed || !window_is_focused(s->win))
        return;
    if (kb.ctrl_s)
        do_save(s);
    else if (kb.ctrl_o)
        do_open(s);
    else if (kb.ctrl_n)
        os_launch_app(&teditor_app);
    else if (kb.ctrl_f)
        do_find(s);
    else if (kb.ctrl && kb.last_scancode == G_KEY)
        do_goto(s);
    else if (kb.last_scancode == F3)
        do_find_next(s);
    else
        return;
    kb.key_pressed = false;
}

/* Used by FileF: open path in an empty TEditor or a new one. */
void teditor_open_file(const char *path) {
    teditor_state_t *t = NULL;
    for (int i = 0; i < MAX_RUNNING_APPS && !t; i++) {
        app_instance_t *a = &running_apps[i];
        teditor_state_t *s = a->state;
        if (a->running && a->desc == &teditor_app && s && s->win && !s->dirty &&
            !s->path[0] && s->edit->text_len == 0)
            t = s;
    }
    if (!t) {
        app_instance_t *a = os_launch_app(&teditor_app);
        if (!a)
            return;
        t = a->state;
    }
    if (t && t->win) {
        load(t, path);
        wm_focus(t->win);
    }
}

app_descriptor teditor_app = {
    .name = "TEDITOR",
    .state_size = sizeof(teditor_state_t),
    .init = teditor_init,
    .on_frame = teditor_on_frame,
};
