/* dialogs.c - message / confirm / input / open / save dialogs. */
#include "ui/dialogs.h"
#include "ui/ctl_internal.h"

#include "config/config.h"
#include "fs/fs.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "ui/window.h"

void (*g_dlg_call_hook)(window *owner, void (*fn)(void *, void *), void *user,
                        void *arg) = NULL;

enum { K_MSG, K_CONFIRM, K_INPUT, K_OPEN, K_SAVE };

typedef struct {
    window *win;
    window *owner;
    int kind;
    dlg_text_cb tcb;
    dlg_bool_cb bcb;
    void *user;
    control *edit, *list, *path, *check;
    bool *check_out; /* dlg_input_check: receives the box on OK */
    char dir[128];
    char ext[4];
    char title[40]; /* callers may pass a stack buffer */
} dlg_t;

#define MAX_DLG 8
static dlg_t *s_open[MAX_DLG];

/* ── plumbing ──────────────────────────────────────────────────────────── */

static void call(window *owner, void (*fn)(void *, void *), void *user,
                 void *arg) {
    if (!fn)
        return;
    if (owner && owner->dyn_state && g_dlg_call_hook)
        g_dlg_call_hook(owner, fn, user, arg);
    else
        fn(user, arg);
}

static void unlink_dlg(dlg_t *d) {
    for (int i = 0; i < MAX_DLG; i++)
        if (s_open[i] == d)
            s_open[i] = NULL;
    if (d->owner && d->owner->modal_child == d->win)
        d->owner->modal_child = NULL;
}

/* close the dialog, then report. text: result for text dialogs */
static void finish(dlg_t *d, bool ok, const char *text) {
    static char result[160];
    if (text) {
        strncpy(result, text, sizeof(result) - 1);
        result[sizeof(result) - 1] = 0;
    }
    if (ok && d->check && d->check_out)
        *d->check_out = ctl_value(d->check) != 0;
    window *owner = d->owner;
    dlg_text_cb tcb = d->tcb;
    dlg_bool_cb bcb = d->bcb;
    void *user = d->user;
    int kind = d->kind;
    unlink_dlg(d);
    wm_unregister(d->win); /* frees the controls */
    kfree(d);
    if (owner)
        wm_focus(owner);
    /* bool travels as a 4-byte stack slot under i386 cdecl, so one
     * (void*, void*) caller serves both callback types */
    if (kind == K_CONFIRM && bcb)
        call(owner, (void (*)(void *, void *))(void (*)(void))bcb, user,
             (void *)(uintptr_t)(ok ? 1 : 0));
    else if (tcb)
        call(owner, (void (*)(void *, void *))(void (*)(void))tcb, user,
             ok ? (void *)result : NULL);
}

static bool on_close_btn(window *w) {
    dlg_t *d = (dlg_t *)w->userdata;
    finish(d, false, NULL);
    return true;
}

static dlg_t *open_dlg(window *owner, int kind, const char *title, int w,
                       int h) {
    dlg_t *d = (dlg_t *)kzalloc(sizeof(dlg_t));
    if (!d)
        return NULL;
    int slot = -1;
    for (int i = 0; i < MAX_DLG; i++)
        if (!s_open[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        kfree(d);
        return NULL;
    }
    int x = (SCREEN_WIDTH - w) / 2, y = (SCREEN_HEIGHT - h) / 3;
    if (owner) {
        x = owner->x + (owner->w - w) / 2;
        y = owner->y + 20;
    }
    window_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.x = x;
    spec.y = y;
    spec.w = w;
    spec.h = h;
    strncpy(d->title, title ? title : "", sizeof(d->title) - 1);
    spec.title = d->title;
    spec.title_color = WHITE;
    spec.bar_color = DARK_GRAY;
    spec.content_color = TH_FACE;
    spec.visible = true;
    spec.on_close = on_close_btn;
    d->win = wm_register(&spec);
    if (!d->win) {
        kfree(d);
        return NULL;
    }
    d->win->anim_state = 0;
    d->win->userdata = d;
    d->owner = owner;
    d->kind = kind;
    if (owner) {
        owner->modal_child = d->win;
        d->win->modal_owner = owner;
    }
    s_open[slot] = d;
    return d;
}

void dlg_owner_destroyed(window *owner) {
    for (int i = 0; i < MAX_DLG; i++) {
        dlg_t *d = s_open[i];
        if (d && d->owner == owner) {
            s_open[i] = NULL;
            d->owner = NULL;
            wm_unregister(d->win);
            kfree(d);
        }
    }
}

static void on_ok(control *c, int ev) {
    (void)ev;
    dlg_t *d = (dlg_t *)c->user;
    finish(d, true, d->edit ? ctl_text(d->edit) : "");
}

static void on_cancel(control *c, int ev) {
    (void)ev;
    finish((dlg_t *)c->user, false, NULL);
}

/* word-wrap msg into out (lines separated by '\n'); returns line count */
static int wrap_text(const char *msg, char *out, int outsz, int cols) {
    int o = 0, lines = 1, col = 0;
    const char *p = msg;
    while (*p && o < outsz - 2) {
        if (*p == '\n') {
            out[o++] = '\n';
            lines++;
            col = 0;
            p++;
            continue;
        }
        /* length of the next word */
        int wl = 0;
        while (p[wl] && p[wl] != ' ' && p[wl] != '\n')
            wl++;
        if (col > 0 && col + wl > cols) {
            out[o++] = '\n';
            lines++;
            col = 0;
            while (*p == ' ')
                p++;
            continue;
        }
        if (wl == 0) { /* a space */
            out[o++] = *p++;
            col++;
            continue;
        }
        while (wl-- > 0 && o < outsz - 2) {
            if (col >= cols) {
                out[o++] = '\n';
                lines++;
                col = 0;
            }
            out[o++] = *p++;
            col++;
        }
    }
    out[o] = 0;
    return lines;
}

static void message_box(window *owner, int kind, const char *title,
                        const char *msg, dlg_bool_cb cb, void *user) {
    char text[512];
    int lines = wrap_text(msg ? msg : "", text, sizeof(text), 38);
    if (lines > 12)
        lines = 12;
    int h = 16 + 8 + lines * 8 + 8 + 13 + 8;
    dlg_t *d = open_dlg(owner, kind, title, 210, h);
    if (!d)
        return;
    d->bcb = cb;
    d->user = user;
    ctl_label(d->win, 8, 8, 192, lines * 8, text, 0);
    int by = 8 + lines * 8 + 8;
    if (kind == K_CONFIRM) {
        control *y = ctl_button(d->win, 48, by, 50, 13, "Yes", 1, on_ok);
        control *n = ctl_button(d->win, 108, by, 50, 13, "No", 2, on_cancel);
        ctl_style_add(y, CS_DEFAULT);
        ctl_style_add(n, CS_CANCEL);
        ctl_focus(y);
    } else {
        control *ok = ctl_button(d->win, 78, by, 50, 13, "OK", 1, on_ok);
        ctl_style_add(ok, CS_DEFAULT | CS_CANCEL);
        ctl_focus(ok);
    }
}

void dlg_message(window *owner, const char *title, const char *msg) {
    message_box(owner, K_MSG, title, msg, NULL, NULL);
}

void dlg_confirm(window *owner, const char *title, const char *msg,
                 dlg_bool_cb cb, void *user) {
    message_box(owner, K_CONFIRM, title, msg, cb, user);
}

void dlg_input_check(window *owner, const char *title, const char *prompt,
                     const char *initial, int max_len, uint32_t edit_style,
                     const char *check_label, bool *checked, dlg_text_cb cb,
                     void *user) {
    int h = check_label ? 82 : 70, by = check_label ? 48 : 36;
    dlg_t *d = open_dlg(owner, K_INPUT, title, 200, h);
    if (!d)
        return;
    d->tcb = cb;
    d->user = user;
    ctl_label(d->win, 8, 6, 180, 8, prompt ? prompt : "", 0);
    d->edit = ctl_edit(d->win, 8, 17, 182, 12, initial ? initial : "", max_len,
                       edit_style & ~ES_MULTILINE, NULL);
    if (check_label) {
        d->check = ctl_check(d->win, 8, 33, 182, check_label,
                             checked && *checked, NULL);
        d->check_out = checked;
    }
    control *ok = ctl_button(d->win, 84, by, 50, 13, "OK", 1, on_ok);
    control *ca = ctl_button(d->win, 140, by, 50, 13, "Cancel", 2, on_cancel);
    ctl_style_add(ok, CS_DEFAULT);
    ctl_style_add(ca, CS_CANCEL);
    ctl_focus(d->edit);
    edit_select_all(d->edit);
}

void dlg_input(window *owner, const char *title, const char *prompt,
               const char *initial, int max_len, uint32_t edit_style,
               dlg_text_cb cb, void *user) {
    dlg_input_check(owner, title, prompt, initial, max_len, edit_style, NULL,
                    NULL, cb, user);
}

/* ── file dialogs ──────────────────────────────────────────────────────── */

#define F_DIR 1u

static void join(char *out, int n, const char *dir, const char *name) {
    if (name[0] == '/')
        snprintf(out, n, "%s", name);
    else if (strcmp(dir, "/") == 0)
        snprintf(out, n, "/%s", name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

static void name83(const dir_entry_t *e, char *out) {
    int j = 0;
    for (int k = 0; k < 8 && e->name[k] != ' '; k++)
        out[j++] = (char)e->name[k];
    if (e->ext[0] != ' ' && !(e->attr & ATTR_DIRECTORY)) {
        out[j++] = '.';
        for (int k = 0; k < 3 && e->ext[k] != ' '; k++)
            out[j++] = (char)e->ext[k];
    }
    out[j] = 0;
}

static void fill_list(dlg_t *d) {
    list_clear(d->list);
    ctl_set_text(d->path, d->dir);
    static char names[160][13];
    static uint8_t isdir[160];
    int n = 0;
    if (strcmp(d->dir, "/") != 0) {
        strcpy(names[n], "..");
        isdir[n++] = 1;
    } else {
        for (int m = 0; m < VFS_MOUNT_COUNT; m++)
            if (fs_drive_mounted(g_vfs_mounts[m].drive_id)) {
                strcpy(names[n], g_vfs_mounts[m].path + 1);
                isdir[n++] = 1;
            }
    }
    uint8_t drv;
    uint16_t cl;
    static dir_entry_t ents[128];
    int cnt = 0;
    if (fs_resolve_dir(d->dir, &drv, &cl) &&
        fs_list_dir(drv, cl, ents, 128, &cnt)) {
        for (int i = 0; i < cnt && n < 160; i++) {
            dir_entry_t *e = &ents[i];
            if (e->name[0] == 0 || e->name[0] == 0xE5 || e->name[0] == '.' ||
                (e->attr & ATTR_VOLUME_ID))
                continue;
            bool dir = (e->attr & ATTR_DIRECTORY) != 0;
            if (!dir && d->ext[0]) {
                char ex[4] = {(char)e->ext[0], (char)e->ext[1], (char)e->ext[2],
                              0};
                for (int k = 2; k >= 0 && ex[k] == ' '; k--)
                    ex[k] = 0;
                if (strcasecmp(ex, d->ext) != 0)
                    continue;
            }
            name83(e, names[n]);
            isdir[n++] = dir;
        }
    }
    /* folders first, then by name */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            bool swap = isdir[j] > isdir[j - 1] ||
                        (isdir[j] == isdir[j - 1] &&
                         strcmp(names[j], names[j - 1]) < 0 &&
                         strcmp(names[j - 1], "..") != 0);
            if (!swap)
                break;
            char t[13];
            strcpy(t, names[j]);
            strcpy(names[j], names[j - 1]);
            strcpy(names[j - 1], t);
            uint8_t b = isdir[j];
            isdir[j] = isdir[j - 1];
            isdir[j - 1] = b;
        }
    for (int i = 0; i < n; i++) {
        char row[20];
        snprintf(row, sizeof(row), isdir[i] ? "[%s]" : "%s", names[i]);
        list_add_data(d->list, row, isdir[i] ? F_DIR : 0);
    }
    if (n)
        list_set_sel(d->list, 0);
}

static void go_dir(dlg_t *d, const char *name) {
    if (strcmp(name, "..") == 0) {
        char *s = strrchr(d->dir, '/');
        if (s && s != d->dir)
            *s = 0;
        else
            strcpy(d->dir, "/");
    } else {
        char nd[128];
        join(nd, sizeof(nd), d->dir, name);
        strncpy(d->dir, nd, sizeof(d->dir) - 1);
    }
    fill_list(d);
}

static void item_name(dlg_t *d, int i, char *out) {
    const char *t = list_get(d->list, i);
    if (t[0] == '[') {
        strncpy(out, t + 1, 12);
        out[12] = 0;
        int n = (int)strlen(out);
        if (n && out[n - 1] == ']')
            out[n - 1] = 0;
    } else {
        strncpy(out, t, 12);
        out[12] = 0;
    }
}

static void save_overwrite_cb(void *user, bool yes) {
    dlg_t *d = (dlg_t *)user;
    if (!yes)
        return;
    char path[160];
    char nm[64];
    strncpy(nm, ctl_text(d->edit), sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = 0;
    join(path, sizeof(path), d->dir, nm);
    finish(d, true, path);
}

static void file_ok(dlg_t *d) {
    char nm[64];
    strncpy(nm, ctl_text(d->edit), sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = 0;
    int sel = list_sel(d->list);
    if (!nm[0]) {
        if (sel >= 0 && (list_data(d->list, sel) & F_DIR)) {
            char n[13];
            item_name(d, sel, n);
            go_dir(d, n);
        }
        return;
    }
    if (strcmp(nm, "..") == 0) {
        go_dir(d, "..");
        ctl_set_text(d->edit, "");
        return;
    }
    char path[160];
    join(path, sizeof(path), d->dir, nm);
    dir_entry_t de;
    uint8_t drv;
    uint16_t cl;
    bool exists = fs_find(path, &de);
    if ((exists && (de.attr & ATTR_DIRECTORY)) ||
        (!exists && nm[0] == '/' && fs_resolve_dir(path, &drv, &cl))) {
        strncpy(d->dir, path, sizeof(d->dir) - 1);
        ctl_set_text(d->edit, "");
        fill_list(d);
        return;
    }
    if (d->kind == K_OPEN) {
        if (!exists) {
            dlg_message(d->win, "Open", "File not found.");
            return;
        }
        finish(d, true, path);
        return;
    }
    /* save: add the default extension, validate, confirm overwrite */
    const char *base = strrchr(nm, '/');
    base = base ? base + 1 : nm;
    if (!strchr(base, '.') && d->ext[0]) {
        int n = (int)strlen(nm);
        snprintf(nm + n, sizeof(nm) - n, ".%s", d->ext);
        join(path, sizeof(path), d->dir, nm);
        ctl_set_text(d->edit, nm);
        exists = fs_find(path, &de);
        base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
    }
    const char *err = NULL;
    if (!fs_validate_name(base, false, &err)) {
        dlg_message(d->win, "Save", err);
        return;
    }
    if (exists) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s already exists.\nReplace it?", base);
        dlg_confirm(d->win, "Save", msg, save_overwrite_cb, d);
        return;
    }
    finish(d, true, path);
}

static void on_file_ok(control *c, int ev) {
    (void)ev;
    file_ok((dlg_t *)c->user);
}

static void on_up(control *c, int ev) {
    (void)ev;
    go_dir((dlg_t *)c->user, "..");
}

static void on_list(control *c, int ev) {
    dlg_t *d = (dlg_t *)c->user;
    int sel = list_sel(c);
    if (sel < 0)
        return;
    bool dir = (list_data(c, sel) & F_DIR) != 0;
    char n[13];
    item_name(d, sel, n);
    if (ev == EV_CHANGE && !dir)
        ctl_set_text(d->edit, n);
    if (ev == EV_SUBMIT) {
        if (dir) {
            go_dir(d, n);
        } else {
            ctl_set_text(d->edit, n);
            file_ok(d);
        }
    }
}

static void file_dialog(window *owner, int kind, const char *title,
                        const char *dir, const char *name, const char *ext,
                        dlg_text_cb cb, void *user) {
    dlg_t *d = open_dlg(owner, kind, title, 230, 172);
    if (!d)
        return;
    d->tcb = cb;
    d->user = user;
    strncpy(d->dir, dir && dir[0] ? dir : "/", sizeof(d->dir) - 1);
    uint8_t drv;
    uint16_t cl;
    if (!fs_resolve_dir(d->dir, &drv, &cl))
        strcpy(d->dir, "/");
    if (ext) {
        strncpy(d->ext, ext, 3);
        d->ext[3] = 0;
    }
    window *w = d->win;
    d->path = ctl_label(w, 6, 5, 180, 12, d->dir, CS_BORDER);
    ctl_button(w, 190, 5, 30, 12, "Up", 3, on_up);
    d->list = ctl_list(w, 6, 21, 214, 98, 0, on_list);
    ctl_label(w, 6, 125, 30, 12, "Name:", 0);
    d->edit =
        ctl_edit(w, 36, 125, 184, 12, name ? name : "", 63, ES_FILENAME, NULL);
    control *ok = ctl_button(w, 114, 142, 50, 13,
                             kind == K_SAVE ? "Save" : "Open", 1, on_file_ok);
    control *ca = ctl_button(w, 170, 142, 50, 13, "Cancel", 2, on_cancel);
    ctl_style_add(ok, CS_DEFAULT);
    ctl_style_add(ca, CS_CANCEL);
    fill_list(d);
    if (kind == K_SAVE) {
        ctl_focus(d->edit);
        edit_select_all(d->edit);
    } else {
        ctl_focus(d->list);
    }
}

void dlg_open_file(window *owner, const char *title, const char *dir,
                   const char *ext, dlg_text_cb cb, void *user) {
    file_dialog(owner, K_OPEN, title ? title : "Open", dir, NULL, ext, cb,
                user);
}

void dlg_save_file(window *owner, const char *title, const char *dir,
                   const char *name, const char *ext, dlg_text_cb cb,
                   void *user) {
    file_dialog(owner, K_SAVE, title ? title : "Save As", dir, name, ext, cb,
                user);
}

/* ── messages from places that must not build windows ──────────────────── */

#define MAX_POSTED 4
static struct {
    char title[40];
    char msg[200];
} s_posted[MAX_POSTED];
static volatile int s_posted_n;

void dlg_post_message(const char *title, const char *msg) {
    if (s_posted_n >= MAX_POSTED)
        return; /* a flood of errors: the first few are enough */
    int i = s_posted_n;
    strncpy(s_posted[i].title, title ? title : "", 39);
    s_posted[i].title[39] = 0;
    strncpy(s_posted[i].msg, msg ? msg : "", 199);
    s_posted[i].msg[199] = 0;
    s_posted_n = i + 1;
}

void dlg_flush_posted(void) {
    for (int i = 0; i < s_posted_n; i++)
        dlg_message(NULL, s_posted[i].title, s_posted[i].msg);
    s_posted_n = 0;
}

/* Draw a box straight into the frame (the system is about to halt). */
void dlg_draw_now(const char *title, const char *msg) {
    char text[512];
    int lines = wrap_text(msg ? msg : "", text, sizeof(text), 38);
    if (lines > 12)
        lines = 12;
    int w = 210, h = 16 + 8 + lines * 8 + 8;
    int x = (SCREEN_WIDTH - w) / 2, y = (SCREEN_HEIGHT - h) / 3;
    gfx_reset_clip();
    fill_rect(x + 3, y + 3, w, h, BLACK);
    fill_rect(x, y, w, h, TH_FACE);
    draw_rect(x, y, w, h, BLACK);
    fill_rect(x + 1, y + 1, w - 2, 14, RED);
    draw_string(x + 4, y + 4, title ? title : "", WHITE, 2);
    const char *p = text;
    for (int l = 0; l < lines; l++) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        draw_text_n(x + 8, y + 24 + l * 8, p, n, BLACK, 2);
        if (!e)
            break;
        p = e + 1;
    }
}
