// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "ui/dialogs.h"
#include "ui/ctl_internal.h"
#include "ui/uifont.h"

#include "config/config.h"
#include "config/runtime_config.h"
#include "drivers/keyboard.h"
#include "fs/fs.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "os/print.h"
#include "ui/fileops.h"
#include "ui/window.h"

void (*g_dlg_call_hook)(window *owner, void (*fn)(void *, void *), void *user,
                        void *arg) = NULL;

enum {
    K_MSG,
    K_CONFIRM,
    K_INPUT,
    K_OPEN,
    K_SAVE,
    K_PRINT,
    K_PREVIEW,
    K_FOLDER,
    K_FORM
};

typedef struct {
    window *win;
    window *owner;
    int kind;
    dlg_text_cb tcb;
    dlg_bool_cb bcb;
    void *user;
    control *edit, *list, *path, *check;
    control *tree;   /* file dialogs: folder tree */
    bool *check_out; /* dlg_input_check: receives the box on OK */
    char dir[FS_PATH_SZ];
    char ext[16];   /* "PIC" or a list: "PIC,BMP" (the first: Save's) */
    control *types; /* Save with a list: the type to save as */
    char typ[4][4]; /* ... the list's types */
    int ntyp;
    char title[40]; /* callers may pass a stack buffer */
    void *xdata;    /* kind-specific (print dialog, preview); freed here */
} dlg_t;

#define MAX_DLG 8
static dlg_t *s_open[MAX_DLG];

// plumbing

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

/* the name's extension is one of the list ("PIC,BMP") */
static bool ext_listed(const char *list, const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    const char *e = dot + 1, *p = list;
    int n = (int)strlen(e);
    while (*p) {
        int k = 0;
        while (p[k] && p[k] != ',')
            k++;
        if (k == n && !strncasecmp(p, e, (size_t)n))
            return true;
        p += p[k] ? k + 1 : k;
    }
    return false;
}

/* the type Save adds: the one chosen, else the list's first */
static const char *save_type(dlg_t *d) {
    if (d->types)
        return d->typ[ctl_value(d->types) & 3];
    return d->ntyp ? d->typ[0] : "";
}

/* close the dialog, then report. text: result for text dialogs */
static void finish(dlg_t *d, bool ok, const char *text) {
    static char result[FS_PATH_SZ];
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
    kfree(d->xdata);
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

#define DLG_BOTTOM 5 /* extra rows below every dialog's buttons */

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
    /* room under the bottom row of buttons (they sat on the edge), if
     * the screen has it */
    if (h + DLG_BOTTOM <= SCREEN_HEIGHT - MENUBAR_H - 1)
        h += DLG_BOTTOM;
    int x = (SCREEN_WIDTH - w) / 2, y = (SCREEN_HEIGHT - h) / 3;
    if (y < MENUBAR_H + 1) /* tall dialog, small screen */
        y = MENUBAR_H + 1;
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
            kfree(d->xdata);
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

void dlg_info(window *owner, const char *title, const char *heading,
              const char *sub, const dlg_row_t *rows, int n,
              const char *footer) {
    char ftext[256];
    int flines = footer ? wrap_text(footer, ftext, sizeof(ftext), 52) : 0;
    const int W = 260, KX = 8, KW = 62, VX = KX + KW + 6, VW = W - VX - 10;
    bool tight = SCREEN_HEIGHT < 300; /* 320x200: must fit under the bar */
    int rh = tight ? 8 : 9, gap = tight ? 3 : 6;
    int h = gap + 10 + (sub ? 10 : 0) + gap + 4 + n * rh + gap + 6 +
            (flines ? flines * 8 + gap : 0) + 13 + gap;
    dlg_t *d = open_dlg(owner, K_MSG, title, W, h + 16);
    if (!d)
        return;
    int y = gap;
    ctl_label(d->win, 0, y, W - 2, 10, heading, LS_CENTER | LS_BOLD);
    y += 10;
    if (sub) {
        ctl_label(d->win, 0, y, W - 2, 8, sub, LS_CENTER);
        y += 10;
    }
    y += gap;
    ctl_group(d->win, KX - 4, y - 4, W - 2 * KX + 2, n * rh + 10, "");
    y += 2;
    for (int i = 0; i < n; i++, y += rh) {
        if (rows[i].key && rows[i].key[0]) {
            char k[24];
            snprintf(k, sizeof(k), "%s:", rows[i].key);
            ctl_label(d->win, KX, y, KW, 8, k, LS_RIGHT);
        }
        ctl_label(d->win, VX, y, VW, 8, rows[i].value ? rows[i].value : "", 0);
    }
    y += gap + 6;
    if (flines) {
        ctl_label(d->win, 8, y, W - 18, flines * 8, ftext, LS_CENTER);
        y += flines * 8 + gap;
    }
    control *ok = ctl_button(d->win, (W - 52) / 2, y, 50, 13, "OK", 1, on_ok);
    ctl_style_add(ok, CS_DEFAULT | CS_CANCEL);
    ctl_focus(ok);
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

// file dialogs

#define F_DIR 1u

static void join(char *out, int n, const char *dir, const char *name) {
    if (name[0] == '/')
        snprintf(out, n, "%s", name);
    else if (strcmp(dir, "/") == 0)
        snprintf(out, n, "/%s", name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

static void fill_list(dlg_t *d) {
    list_clear(d->list);
    ctl_set_text(d->path, d->dir);
    static char names[160][FS_NAME_SZ];
    static char got[128][FS_NAME_SZ];
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
        fs_list_dir_names(drv, cl, ents, got, 128, &cnt)) {
        for (int i = 0; i < cnt && n < 160; i++) {
            dir_entry_t *e = &ents[i];
            if (e->name[0] == 0 || e->name[0] == 0xE5 || e->name[0] == '.' ||
                (e->attr & ATTR_VOLUME_ID))
                continue;
            bool dir = (e->attr & ATTR_DIRECTORY) != 0;
            if (!dir && d->kind == K_FOLDER)
                continue;
            if (!dir && d->ext[0] &&
                !ext_listed(d->types ? d->typ[ctl_value(d->types) & 3] : d->ext,
                            got[i]))
                continue;
            strcpy(names[n], got[i]);
            isdir[n++] = dir;
        }
    }
    /* folders first, then by name */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            bool swap = isdir[j] > isdir[j - 1] ||
                        (isdir[j] == isdir[j - 1] &&
                         strcasecmp(names[j], names[j - 1]) < 0 &&
                         strcmp(names[j - 1], "..") != 0);
            if (!swap)
                break;
            char t[FS_NAME_SZ];
            strcpy(t, names[j]);
            strcpy(names[j], names[j - 1]);
            strcpy(names[j - 1], t);
            uint8_t b = isdir[j];
            isdir[j] = isdir[j - 1];
            isdir[j - 1] = b;
        }
    for (int i = 0; i < n; i++) {
        char row[FS_NAME_SZ + 4];
        snprintf(row, sizeof(row), isdir[i] ? "[%s]" : "%s", names[i]);
        list_add_data(d->list, row, isdir[i] ? F_DIR : 0);
    }
    if (n)
        list_set_sel(d->list, 0);
    if (d->tree) /* follow in the tree, without re-triggering it */
        tree_set_sel(d->tree, strcmp(d->dir, "/")
                                  ? tree_find_path(d->tree, d->dir)
                                  : -1);
}

static void go_dir(dlg_t *d, const char *name) {
    if (strcmp(name, "..") == 0) {
        char *s = strrchr(d->dir, '/');
        if (s && s != d->dir)
            *s = 0;
        else
            strcpy(d->dir, "/");
    } else {
        char nd[FS_PATH_SZ];
        join(nd, sizeof(nd), d->dir, name);
        strncpy(d->dir, nd, sizeof(d->dir) - 1);
    }
    fill_list(d);
}

static void item_name(dlg_t *d, int i, char *out) {
    const char *t = list_get(d->list, i);
    if (t[0] == '[') {
        strncpy(out, t + 1, FS_NAME_MAX);
        out[FS_NAME_MAX] = 0;
        int n = (int)strlen(out);
        if (n && out[n - 1] == ']')
            out[n - 1] = 0;
    } else {
        strncpy(out, t, FS_NAME_MAX);
        out[FS_NAME_MAX] = 0;
    }
}

static void save_overwrite_cb(void *user, bool yes) {
    dlg_t *d = (dlg_t *)user;
    if (!yes)
        return;
    char path[FS_PATH_SZ];
    char nm[FS_PATH_SZ];
    strncpy(nm, ctl_text(d->edit), sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = 0;
    join(path, sizeof(path), d->dir, nm);
    finish(d, true, path);
}

static void file_ok(dlg_t *d) {
    if (d->kind ==
        K_FOLDER) { /* the folder picked in the list, else this one */
        char path[FS_PATH_SZ];
        int sel = list_sel(d->list);
        snprintf(path, sizeof(path), "%s", d->dir);
        if (sel > 0 || (sel == 0 && strcmp(list_get(d->list, 0), "[..]"))) {
            char n[FS_NAME_SZ];
            item_name(d, sel, n);
            join(path, sizeof(path), d->dir, n);
        }
        finish(d, true, path);
        return;
    }
    char nm[FS_PATH_SZ];
    strncpy(nm, ctl_text(d->edit), sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = 0;
    int sel = list_sel(d->list);
    if (!nm[0]) {
        if (sel >= 0 && (list_data(d->list, sel) & F_DIR)) {
            char n[FS_NAME_SZ];
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
    char path[FS_PATH_SZ];
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
    /* no extension, or (with a Type list) one that isn't of the list:
     * the type's is added */
    if (d->ext[0] &&
        (!strchr(base, '.') || (d->types && !ext_listed(d->ext, base)))) {
        int n = (int)strlen(nm);
        snprintf(nm + n, sizeof(nm) - n, ".%s", save_type(d));
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
        char msg[FS_NAME_SZ + 32];
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
    char n[FS_NAME_SZ];
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

static void on_dir_tree(control *c, int ev) {
    dlg_t *d = (dlg_t *)c->user;
    char path[FS_PATH_SZ];
    if (ev == EV_EXPAND) {
        int node = tree_event_node(c);
        tree_path(c, node, path, sizeof(path));
        static char names[64][FS_NAME_SZ];
        int n = fo_list_dirs(path, names, 64, false);
        for (int i = 0; i < n; i++)
            tree_add(c, node, names[i], 0, true);
    } else if (ev == EV_CHANGE && tree_sel(c) >= 0) {
        tree_path(c, tree_sel(c), path, sizeof(path));
        if (strcmp(path, d->dir)) {
            strncpy(d->dir, path, sizeof(d->dir) - 1);
            fill_list(d);
        }
    }
}

/* another type: the name's extension follows (if it is one of the list),
 * and the files shown */
static void on_type(control *c, int ev) {
    dlg_t *d = (dlg_t *)c->user;
    if (ev != EV_CHANGE)
        return;
    char nm[FS_PATH_SZ];
    strncpy(nm, ctl_text(d->edit), sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = 0;
    char *dot = strrchr(nm, '.');
    if (dot && !strchr(dot, '/') && ext_listed(d->ext, nm)) {
        snprintf(dot + 1, sizeof(nm) - (size_t)(dot + 1 - nm), "%s",
                 save_type(d));
        ctl_set_text(d->edit, nm);
    }
    fill_list(d);
}

static void file_dialog(window *owner, int kind, const char *title,
                        const char *dir, const char *name, const char *ext,
                        dlg_text_cb cb, void *user) {
    dlg_t *d = open_dlg(owner, kind, title, 310, 172);
    if (!d)
        return;
    d->tcb = cb;
    d->user = user;
    strncpy(d->dir, dir && dir[0] ? dir : "/", sizeof(d->dir) - 1);
    uint8_t drv;
    uint16_t cl;
    if (!fs_resolve_dir(d->dir, &drv, &cl))
        strcpy(d->dir, "/");
    if (ext)
        strncpy(d->ext, ext, sizeof(d->ext) - 1);
    for (const char *e = d->ext; *e && d->ntyp < 4;) { /* the list */
        int k = 0;
        while (e[k] && e[k] != ',')
            k++;
        snprintf(d->typ[d->ntyp++], sizeof(d->typ[0]), "%.*s", k < 3 ? k : 3,
                 e);
        e += e[k] ? k + 1 : k;
    }
    window *w = d->win;
    d->path = ctl_label(w, 6, 5, 260, 12, d->dir, CS_BORDER);
    ctl_set_tooltip(ctl_button(w, 270, 5, 30, 12, "Up", 3, on_up),
                    "Parent folder");
    d->tree = ctl_tree(w, 6, 21, 78, 98, TS_FOLDERS, on_dir_tree);
    if (fs_drive_mounted(DRIVE_HDA))
        tree_add(d->tree, -1, "HDA", 0, true);
    for (int m = 0; m < VFS_MOUNT_COUNT; m++)
        if (fs_drive_mounted(g_vfs_mounts[m].drive_id))
            tree_add(d->tree, -1, g_vfs_mounts[m].path + 1, 0, true);
    d->list = ctl_list(w, 86, 21, 214, 98, 0, on_list);
    if (kind == K_FOLDER) {
        ctl_label(w, 6, 125, 294, 12,
                  "Pick a folder (or open one), then Choose.", 0);
    } else {
        ctl_label(w, 6, 125, 30, 12, "Name:", 0);
        d->edit = ctl_edit(w, 36, 125, 264, 12, name ? name : "", 255,
                           ES_FILENAME, NULL);
    }
    if (kind == K_SAVE && d->ntyp > 1) { /* Save as: which type */
        ctl_label(w, 6, 143, 30, 12, "Type:", 0);
        d->types = ctl_combo(w, 36, 142, 150, on_type);
        d->types->user = d;
        for (int i = 0; i < d->ntyp; i++) {
            char t[40];
            snprintf(t, sizeof(t), "%s (.%s)", fo_type_of(d->typ[i]),
                     d->typ[i]);
            list_add(d->types, t);
        }
        list_set_sel(d->types, 0);
    }
    control *ok = ctl_button(w, 194, 142, 50, 13,
                             kind == K_SAVE     ? "Save"
                             : kind == K_FOLDER ? "Choose"
                                                : "Open",
                             1, on_file_ok);
    control *ca = ctl_button(w, 250, 142, 50, 13, "Cancel", 2, on_cancel);
    ctl_style_add(ok, CS_DEFAULT);
    ctl_style_add(ca, CS_CANCEL);
    fill_list(d);
    if (kind == K_SAVE && d->edit) {
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

void dlg_choose_folder(window *owner, const char *title, const char *dir,
                       dlg_text_cb cb, void *user) {
    file_dialog(owner, K_FOLDER, title ? title : "Choose a Folder", dir, NULL,
                NULL, cb, user);
}

// forms: dialogs built by the caller

window *dlg_form(window *owner, const char *title, int w, int h,
                 uint32_t state_size) {
    dlg_t *d = open_dlg(owner, K_FORM, title, w, h);
    if (!d)
        return NULL;
    d->xdata = kzalloc(state_size ? state_size : 4);
    if (!d->xdata) {
        finish(d, false, NULL);
        return NULL;
    }
    return d->win;
}

void *dlg_form_state(window *form) {
    return form ? ((dlg_t *)form->userdata)->xdata : NULL;
}

void dlg_form_close(window *form) {
    if (form)
        finish((dlg_t *)form->userdata, false, NULL);
}

// messages from places that must not build windows

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
    if (y < MENUBAR_H + 1) /* tall dialog, small screen */
        y = MENUBAR_H + 1;
    gfx_reset_clip();
    fill_rect(x + 3, y + 3, w, h, BLACK);
    fill_rect(x, y, w, h, TH_FACE);
    draw_rect(x, y, w, h, BLACK);
    fill_rect(x + 1, y + 1, w - 2, 14, RED);
    draw_string(x + 4, y + 3, title ? title : "", WHITE, UI_FONT_BOLD);
    const char *p = text;
    for (int l = 0; l < lines; l++) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        draw_text_n(x + 8, y + 24 + l * 8, p, n, BLACK, UI_FONT_SMALL);
        if (!e)
            break;
        p = e + 1;
    }
}

// Print and Print Preview

typedef struct {
    print_doc_t doc;
    int pages;
    control *copies, *all, *range, *from, *to, *print, *status;
} prn_dlg_t;

typedef struct {
    print_doc_t doc;
    int pages, page; /* 1-based */
    bool zoom;       /* readable text instead of the whole page */
    int top;         /* zoomed: first line shown */
    control *view, *label, *prev, *next, *zoom_btn;
} prn_prev_t;

static int field_int(control *c, int def) {
    const char *t = ctl_text(c);
    int v = 0, n = 0;
    for (; *t >= '0' && *t <= '9'; t++, n++)
        v = v * 10 + (*t - '0');
    return n && !*t ? v : def;
}

/* paper: letter, 8.5 x 11 inches; text: 80 columns from 0.25" and 60
 * lines from 0.5" at 10 characters and 6 lines an inch */
#define PAPER_W 85 /* tenths of an inch */
#define PAPER_H 110

static void paint_page(control *c, int x, int y, int w, int h) {
    dlg_t *d = c->user;
    prn_prev_t *p = d->xdata;
    fill_rect(x, y, w, h, DARK_GRAY);
    int f = FONT_MONO; /* 4x6 in 5-pixel cells: the closest to a printer */

    if (p->zoom && p->doc.kind == PRINT_DOC_TEXT) { /* readable */
        static char lines[PRINT_LINES][PRINT_COLS + 1];
        int n = print_text_page(p->doc.text, p->doc.len, p->page, lines);
        int pw = PRINT_COLS * 5 + 16, px = x + (w - pw) / 2;
        if (pw > w)
            px = x;
        fill_rect(px, y, pw, h, WHITE);
        int rows = (h - 8) / 7;
        if (p->top > n - rows)
            p->top = n - rows;
        if (p->top < 0)
            p->top = 0;
        for (int i = 0; i < rows && p->top + i < n; i++)
            draw_string(px + 8, y + 4 + i * 7, lines[p->top + i], BLACK, f);
        return;
    }

    /* the whole page, to scale */
    int ph = h - 8, pw = ph * PAPER_W / PAPER_H;
    if (pw > w - 8) {
        pw = w - 8;
        ph = pw * PAPER_H / PAPER_W;
    }
    int px = x + (w - pw) / 2, py = y + (h - ph) / 2;
    fill_rect(px + 3, py + 3, pw, ph, BLACK); /* shadow */
    fill_rect(px, py, pw, ph, WHITE);
    draw_rect(px, py, pw, ph, BLACK);

    if (p->doc.kind == PRINT_DOC_TEXT) {
        /* each word as a grey bar, where the printer will put it */
        static char lines[PRINT_LINES][PRINT_COLS + 1];
        int n = print_text_page(p->doc.text, p->doc.len, p->page, lines);
        for (int i = 0; i < n; i++) {
            int ly = py + (5 + i * 10 / 6) * ph / PAPER_H;
            int lh = ph / (PAPER_H * 6 / 10) - 1;
            if (lh < 1)
                lh = 1;
            const char *l = lines[i];
            for (int col = 0; l[col];) {
                if (l[col] == ' ') {
                    col++;
                    continue;
                }
                int e = col;
                while (l[e] && l[e] != ' ')
                    e++;
                int x0 =
                    px + (int)((25 + col * 10) * (long)pw / (PAPER_W * 10));
                int x1 = px + (int)((25 + e * 10) * (long)pw / (PAPER_W * 10));
                fill_rect(x0, ly, x1 - x0 > 0 ? x1 - x0 : 1, lh, DARK_GRAY);
                col = e;
            }
        }
        return;
    }

    /* a picture: the printer's dots, from the top-left margin */
    int type = g_cfg.printer_type % PRN_TYPES, sx, sy;
    print_image_scale(p->doc.w, p->doc.h, type, &sx, &sy);
    int dpi = type == PRN_PCL ? 100 : 80;
    if (type == PRN_TEXT) { /* characters: 10 an inch, 6 lines an inch */
        int cw = (p->doc.w + PRINT_COLS - 1) / PRINT_COLS;
        if (cw < 1)
            cw = 1;
        int chh = cw * 2;
        for (int yy = 0; yy < p->doc.h; yy += chh)
            for (int xx = 0; xx < p->doc.w; xx += cw) {
                uint32_t sum = 0, k = 0;
                for (int a = yy; a < yy + chh && a < p->doc.h; a++)
                    for (int b = xx; b < xx + cw && b < p->doc.w; b++) {
                        sum += print_grey(p->doc.pix[a * p->doc.stride + b]);
                        k++;
                    }
                int ci = xx / cw, li = yy / chh;
                if (li >= PRINT_LINES)
                    break;
                int x0 = px + (25 + ci * 10) * pw / (PAPER_W * 10);
                int x1 = px + (25 + ci * 10 + 10) * pw / (PAPER_W * 10);
                int y0 = py + (50 + li * 100 / 6) * ph / (PAPER_H * 10);
                int y1 = py + (50 + (li + 1) * 100 / 6) * ph / (PAPER_H * 10);
                uint8_t g = (uint8_t)(k ? sum / k : 255);
                fill_rect(x0, y0, x1 - x0, y1 - y0,
                          g < 64    ? BLACK
                          : g < 128 ? DARK_GRAY
                          : g < 200 ? LIGHT_GRAY
                                    : WHITE);
            }
        return;
    }
    /* printed size in tenths of an inch */
    int iw = p->doc.w * sx * 10 / dpi, ih = p->doc.h * sy * 10 / dpi;
    int ox = px + 3 * pw / PAPER_W, oy = py + 3 * ph / PAPER_H;
    int vw = iw * pw / PAPER_W, vh = ih * ph / PAPER_H;
    for (int vy = 0; vy < vh && oy + vy < py + ph - 1; vy++) {
        int dy = (int)((long)vy * p->doc.h * sy / vh); /* printer dot row */
        for (int vx = 0; vx < vw && ox + vx < px + pw - 1; vx++) {
            int dx = (int)((long)vx * p->doc.w * sx / vw);
            uint8_t g =
                print_grey(p->doc.pix[(dy / sy) * p->doc.stride + dx / sx]);
            if (print_ink(g, dx, dy))
                draw_dot(ox + vx, oy + vy, BLACK);
        }
    }
}

static void prev_update(dlg_t *d) {
    prn_prev_t *p = d->xdata;
    char b[48];
    snprintf(b, sizeof(b), "Page %d of %d", p->page, p->pages);
    ctl_set_text(p->label, b);
    ctl_enable(p->prev, p->page > 1);
    ctl_enable(p->next, p->page < p->pages);
    ctl_set_text(p->zoom_btn, p->zoom ? "Whole Page" : "Zoom");
    wm_invalidate(d->win);
}

static void on_prev_btn(control *c, int ev) {
    dlg_t *d = c->user;
    prn_prev_t *p = d->xdata;
    if (ev != EV_CLICK)
        return;
    if (c->id == 1 && p->page > 1)
        p->page--, p->top = 0;
    else if (c->id == 2 && p->page < p->pages)
        p->page++, p->top = 0;
    else if (c->id == 3)
        p->zoom = !p->zoom;
    else if (c->id == 4) {
        finish(d, false, NULL);
        return;
    }
    prev_update(d);
}

static void on_page_view(control *c, int ev) {
    dlg_t *d = c->user;
    prn_prev_t *p = d->xdata;
    if (ev == EV_DBLCLICK && p->doc.kind == PRINT_DOC_TEXT) {
        p->zoom = !p->zoom;
    } else if (ev == EV_KEY) {
        uint8_t sc = kb.last_scancode;
        if (sc == PAGE_DOWN || (!p->zoom && sc == RIGHT_ARROW)) {
            if (p->page < p->pages)
                p->page++, p->top = 0;
        } else if (sc == PAGE_UP || (!p->zoom && sc == LEFT_ARROW)) {
            if (p->page > 1)
                p->page--, p->top = 0;
        } else if (sc == DOWN_ARROW && p->zoom) {
            p->top += 3;
        } else if (sc == UP_ARROW && p->zoom) {
            p->top -= 3;
        } else {
            return;
        }
    } else {
        return;
    }
    prev_update(d);
}

static void open_preview(dlg_t *owner, const print_doc_t *doc) {
    int w = SCREEN_WIDTH - 40, h = SCREEN_HEIGHT - MENUBAR_H - 24;
    dlg_t *d = open_dlg(owner->win, K_PREVIEW, "Print Preview", w, h);
    if (!d)
        return;
    prn_prev_t *p = kzalloc(sizeof(prn_prev_t));
    if (!p) {
        finish(d, false, NULL);
        return;
    }
    d->xdata = p;
    d->win->x = 20;
    d->win->y = 2;
    p->doc = *doc;
    p->pages =
        doc->kind == PRINT_DOC_TEXT ? print_text_pages(doc->text, doc->len) : 1;
    p->page = 1;
    int ch = h - 16;
    p->view = ctl_custom(d->win, 2, 2, w - 6, ch - 22, paint_page, on_page_view,
                         CS_FOCUSABLE);
    p->prev = ctl_button(d->win, 4, ch - 17, 40, 13, "< Prev", 1, on_prev_btn);
    p->next = ctl_button(d->win, 48, ch - 17, 40, 13, "Next >", 2, on_prev_btn);
    p->label = ctl_label(d->win, 96, ch - 17, 90, 13, "", 0);
    p->zoom_btn =
        ctl_button(d->win, w - 128, ch - 17, 64, 13, "Zoom", 3, on_prev_btn);
    ctl_enable(p->zoom_btn, doc->kind == PRINT_DOC_TEXT);
    control *cl =
        ctl_button(d->win, w - 60, ch - 17, 52, 13, "Close", 4, on_prev_btn);
    ctl_style_add(cl, CS_DEFAULT | CS_CANCEL);
    d->win->lazy_custom = true;
    prev_update(d);
    ctl_focus(p->view);
}

static void prn_refresh(dlg_t *d) {
    prn_dlg_t *p = d->xdata;
    char b[96];
    snprintf(b, sizeof(b), "%s on %s: %s",
             PRN_TYPE_NAMES[g_cfg.printer_type % PRN_TYPES], print_port_name(),
             print_status_text());
    ctl_set_text(p->status, b);
    ctl_enable(p->print, print_available());
}

static void on_prn_btn(control *c, int ev) {
    dlg_t *d = c->user;
    prn_dlg_t *p = d->xdata;
    if (ev != EV_CLICK && !(ev == EV_CHANGE && (c == p->all || c == p->range)))
        return;
    if (c == p->all || c == p->range) {
        bool r = ctl_value(p->range) != 0;
        if (p->from) {
            ctl_enable(p->from, r);
            ctl_enable(p->to, r);
        }
        return;
    }
    if (c->id == 3) { /* Preview */
        open_preview(d, &p->doc);
        return;
    }
    if (c->id == 2) { /* Cancel */
        finish(d, false, NULL);
        return;
    }
    /* Print */
    int copies = field_int(p->copies, 0);
    if (copies < 1 || copies > 99) {
        dlg_message(d->win, "Print", "Copies: 1 to 99.");
        return;
    }
    int from = 0, to = 0;
    if (p->from && ctl_value(p->range)) {
        from = field_int(p->from, 0);
        to = field_int(p->to, 0);
        if (from < 1 || to < from || to > p->pages) {
            char m[48];
            snprintf(m, sizeof(m), "Pages: 1 to %d.", p->pages);
            dlg_message(d->win, "Print", m);
            return;
        }
    }
    int id = p->doc.kind == PRINT_DOC_TEXT
                 ? print_submit_text(p->doc.name, p->doc.text, p->doc.len,
                                     copies, from, to)
                 : print_submit_image(p->doc.name, p->doc.pix, p->doc.w,
                                      p->doc.h, p->doc.stride, copies);
    if (id < 0) {
        dlg_message(d->win, "Print",
                    print_available() ? "The print queue is full, or there "
                                        "is not enough memory."
                                      : "There is no printer port.");
        return;
    }
    finish(d, true, NULL);
}

void dlg_print(window *owner, const print_doc_t *doc) {
    if (!doc)
        return;
    bool text = doc->kind == PRINT_DOC_TEXT;
    int h = text ? 112 : 84;
    dlg_t *d = open_dlg(owner, K_PRINT, "Print", 230, h + 16);
    if (!d)
        return;
    prn_dlg_t *p = kzalloc(sizeof(prn_dlg_t));
    if (!p) {
        finish(d, false, NULL);
        return;
    }
    d->xdata = p;
    p->doc = *doc;
    p->pages = text ? print_text_pages(doc->text, doc->len) : 1;
    window *w = d->win;
    p->status = ctl_label(w, 8, 6, 214, 9, "", 0);
    ctl_label(w, 8, 22, 50, 12, "Copies:", 0);
    p->copies = ctl_edit(w, 60, 22, 26, 12, "1", 2, ES_DECIMAL, NULL);
    int y = 40;
    if (text) {
        char b[40];
        snprintf(b, sizeof(b), "All %d page%s", p->pages,
                 p->pages == 1 ? "" : "s");
        p->all = ctl_radio(w, 8, y, 120, b, 1, true, on_prn_btn);
        p->range = ctl_radio(w, 8, y + 14, 50, "Pages", 1, false, on_prn_btn);
        p->from = ctl_edit(w, 60, y + 13, 26, 12, "1", 3, ES_DECIMAL, NULL);
        ctl_label(w, 90, y + 13, 14, 12, "to", 0);
        snprintf(b, sizeof(b), "%d", p->pages);
        p->to = ctl_edit(w, 106, y + 13, 26, 12, b, 3, ES_DECIMAL, NULL);
        ctl_enable(p->from, false);
        ctl_enable(p->to, false);
        y += 34;
    }
    control *pv = ctl_button(w, 8, y, 60, 13, "Preview...", 3, on_prn_btn);
    ctl_set_tooltip(pv, "See the pages before they print");
    p->print = ctl_button(w, 112, y, 52, 13, "Print", 1, on_prn_btn);
    control *ca = ctl_button(w, 170, y, 52, 13, "Cancel", 2, on_prn_btn);
    ctl_style_add(p->print, CS_DEFAULT);
    ctl_style_add(ca, CS_CANCEL);
    prn_refresh(d);
    ctl_focus(print_available() ? p->print : ca);
}
