/* Compress/extract dialogs for fs/zip.c. Jobs run on submit and draw progress
 * directly to screen while other work pauses, like folder copying. */
#include "drivers/gpu.h"
#include "fs/fs.h"
#include "fs/zip.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "os/watchdog.h"
#include "ui/dialogs.h"
#include "ui/fileops.h"
#include "ui/uifont.h"

bool fo_is_zip(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".zip");
}

// progress box

typedef struct {
    const char *title;
    uint32_t last_ms;
    bool drawn;
} prog_t;

static void progress(void *user, const char *name, uint32_t done,
                     uint32_t total) {
    prog_t *p = (prog_t *)user;
    uint32_t now = time_millis();
    if (p->drawn && now - p->last_ms < 120)
        return;
    p->last_ms = now;
    p->drawn = true;
    int w = SCREEN_WIDTH < 400 ? 200 : 240, h = 52;
    int x = (SCREEN_WIDTH - w) / 2, y = (SCREEN_HEIGHT - h) / 3;
    gfx_reset_clip();
    fill_rect(x + 3, y + 3, w, h, BLACK);
    fill_rect(x, y, w, h, TH_FACE);
    draw_rect(x, y, w, h, BLACK);
    fill_rect(x + 1, y + 1, w - 2, 12, DARK_GRAY);
    draw_string(x + 4, y + 3, p->title, WHITE, UI_FONT_BOLD);
    gfx_set_clip(x + 6, y + 16, w - 12, 10);
    draw_string(x + 6, y + 17, name ? name : "", BLACK, UI_FONT_SMALL);
    gfx_reset_clip();
    int bw = w - 12, bx = x + 6, by = y + 32;
    fill_rect(bx, by, bw, 10, WHITE);
    draw_rect(bx, by, bw, 10, BLACK);
    uint32_t f = !total              ? 0
                 : total > 0x1000000 ? done / (total / (uint32_t)(bw - 2))
                                     : done * (uint32_t)(bw - 2) / total;
    if (f > (uint32_t)(bw - 2))
        f = (uint32_t)(bw - 2);
    if (f)
        fill_rect(bx + 1, by + 1, (int)f, 8, BLUE);
    gpu_blit_rows(y, y + h + 3);
}

static void fmt_size(uint32_t n, char *out, int sz) {
    if (n >= 10u * 1024 * 1024)
        snprintf(out, sz, "%u MB", n / (1024 * 1024));
    else if (n >= 10240)
        snprintf(out, sz, "%u KB", n / 1024);
    else
        snprintf(out, sz, "%u bytes", n);
}

static void parent_of(const char *path, char *out, int sz) {
    snprintf(out, sz, "%s", path);
    char *s = strrchr(out, '/');
    if (s && s != out)
        *s = 0;
    else if (s)
        s[1] = 0;
}

// Compress

typedef struct {
    window *win, *owner;
    char src[FS_PATH_SZ];
    control *name, *dir, *level, *hidden;
    fo_done_cb done;
    void *user;
} cz_t;

static int s_last_level = ZIP_NORMAL; /* what was picked last time */

static void cz_browse_got(void *user, const char *path) {
    cz_t *c = (cz_t *)user;
    if (path)
        ctl_set_text(c->dir, path);
}

static void cz_browse(control *b, int ev) {
    (void)ev;
    cz_t *c = (cz_t *)b->user;
    dlg_choose_folder(c->win, "Save the Archive In", ctl_text(c->dir),
                      cz_browse_got, c);
}

static void cz_cancel(control *b, int ev) {
    (void)ev;
    dlg_form_close(((cz_t *)b->user)->win);
}

static void cz_run(cz_t *c, bool replace) {
    char dir[FS_PATH_SZ], name[FS_NAME_SZ + 4], zip[FS_PATH_SZ];
    snprintf(dir, sizeof(dir), "%s", ctl_text(c->dir));
    snprintf(name, sizeof(name), "%s", ctl_text(c->name));
    fo_join(dir, name, zip, sizeof(zip));
    if (replace && fo_delete(zip)) {
        dlg_message(c->win, "Compress",
                    "The old archive could not be removed.");
        return;
    }
    int level = list_sel(c->level);
    bool hidden = ctl_value(c->hidden) != 0;
    s_last_level = level;
    /* everything needed is copied: the dialog goes now */
    char src[FS_PATH_SZ];
    snprintf(src, sizeof(src), "%s", c->src);
    window *owner = c->owner;
    fo_done_cb done = c->done;
    void *user = c->user;
    dlg_form_close(c->win);

    wd_busy();
    prog_t p = {"Compressing", 0, false};
    zip_stats_t st;
    const char *err = zip_create(src, zip, level, hidden, progress, &p, &st);
    wm_invalidate_all();
    fo_changed(dir);
    if (err) {
        fo_report(owner, "Compress", err);
        return;
    }
    char a[24], b[24], msg[FS_NAME_SZ + 160];
    fmt_size(st.in_bytes, a, sizeof(a));
    fmt_size(st.out_bytes, b, sizeof(b));
    int pct = st.in_bytes && st.out_bytes < st.in_bytes
                  ? (int)(100 - (st.in_bytes > 0x1000000
                                     ? st.out_bytes / (st.in_bytes / 100)
                                     : st.out_bytes * 100 / st.in_bytes))
                  : 0;
    int l = snprintf(msg, sizeof(msg), "%s is ready.\n%d file%s: %s -> %s",
                     name, st.files, st.files == 1 ? "" : "s", a, b);
    if (pct)
        snprintf(msg + l, sizeof(msg) - l, " (%d%% smaller).", pct);
    else
        snprintf(msg + l, sizeof(msg) - l, ".");
    if (st.stored_only) {
        int l = (int)strlen(msg);
        snprintf(msg + l, sizeof(msg) - l,
                 "\nNot enough memory to compress: the files are stored "
                 "as they are.");
    }
    dlg_message(owner, "Compress", msg);
    if (done)
        done(user);
}

static void cz_replace_cb(void *user, bool yes) {
    if (yes)
        cz_run((cz_t *)user, true);
}

static void cz_ok(control *b, int ev) {
    (void)ev;
    cz_t *c = (cz_t *)b->user;
    char name[FS_NAME_SZ + 4];
    snprintf(name, sizeof(name), "%s", ctl_text(c->name));
    int n = (int)strlen(name);
    while (n && name[n - 1] == ' ')
        name[--n] = 0;
    if (!strchr(name, '.') && n + 4 <= FS_NAME_MAX) {
        strcat(name, ".zip");
        ctl_set_text(c->name, name);
    }
    const char *why = NULL;
    if (!fs_validate_name(name, false, &why)) {
        dlg_message(c->win, "Compress", why ? why : "That name won't do.");
        return;
    }
    uint8_t drv;
    uint16_t cl;
    if (!fs_resolve_dir(ctl_text(c->dir), &drv, &cl)) {
        dlg_message(c->win, "Compress", "That folder does not exist.");
        return;
    }
    char zip[FS_PATH_SZ];
    fo_join(ctl_text(c->dir), name, zip, sizeof(zip));
    bool is_dir;
    if (fo_stat(zip, &is_dir, NULL)) {
        if (is_dir) {
            dlg_message(c->win, "Compress",
                        "There is a folder with that name there.");
            return;
        }
        char m[FS_NAME_SZ + 40];
        snprintf(m, sizeof(m), "%s already exists.\nReplace it?", name);
        dlg_confirm(c->win, "Compress", m, cz_replace_cb, c);
        return;
    }
    cz_run(c, false);
}

void fo_ask_compress(window *owner, const char *path, fo_done_cb done,
                     void *user) {
    bool is_dir;
    if (!fo_stat(path, &is_dir, NULL)) {
        dlg_message(owner, "Compress", "Item not found.");
        return;
    }
    if (fo_protected(path)) {
        dlg_message(owner, "Compress", "This item is protected.");
        return;
    }
    window *w = dlg_form(owner, "Compress", 230, 118, sizeof(cz_t));
    if (!w)
        return;
    cz_t *c = (cz_t *)dlg_form_state(w);
    c->win = w;
    c->owner = owner;
    c->done = done;
    c->user = user;
    snprintf(c->src, sizeof(c->src), "%s", path);

    char nm[FS_NAME_SZ], zn[FS_NAME_SZ + 4], dir[FS_PATH_SZ];
    if (!fs_name_of(path, nm))
        snprintf(nm, sizeof(nm), "%s", fo_base(path));
    snprintf(zn, sizeof(zn), "%s", nm);
    if (!is_dir) { /* "Report.txt" -> "Report.zip" */
        char *dot = strrchr(zn, '.');
        if (dot && dot != zn)
            *dot = 0;
    }
    if ((int)strlen(zn) + 4 > FS_NAME_MAX)
        zn[FS_NAME_MAX - 4] = 0;
    strcat(zn, ".zip");
    parent_of(path, dir, sizeof(dir));
    if (fo_in_trash(path) || fo_is_trash(dir))
        snprintf(dir, sizeof(dir), "/DESKTOP");

    char head[FS_NAME_SZ + 16];
    snprintf(head, sizeof(head), "Make an archive of \"%s\"", nm);
    ctl_label(w, 8, 5, 214, 10, head, 0);
    ctl_label(w, 8, 20, 48, 10, "Name:", 0);
    c->name = ctl_edit(w, 58, 18, 164, 12, zn, FS_NAME_MAX, ES_FILENAME, NULL);
    ctl_label(w, 8, 35, 48, 10, "Save in:", 0);
    c->dir = ctl_edit(w, 58, 33, 118, 12, dir, FS_PATH_MAX, ES_FILENAME, NULL);
    control *br = ctl_button(w, 180, 33, 42, 12, "Browse", 3, cz_browse);
    br->user = c;
    ctl_label(w, 8, 51, 48, 10, "Compress:", 0);
    c->level = ctl_combo(w, 58, 49, 100, NULL);
    list_add(c->level, "None (store)");
    list_add(c->level, "Fast");
    list_add(c->level, "Normal");
    list_add(c->level, "Best (slow)");
    list_set_sel(c->level, s_last_level);
    c->hidden = ctl_check(w, 8, 66, 214, "Include hidden files", false, NULL);
    control *ok = ctl_button(w, 116, 86, 52, 13, "Compress", 1, cz_ok);
    control *ca = ctl_button(w, 172, 86, 50, 13, "Cancel", 2, cz_cancel);
    ok->user = ca->user = c;
    ctl_style_add(ok, CS_DEFAULT);
    ctl_style_add(ca, CS_CANCEL);
    ctl_focus(c->name);
    /* the part before ".zip", ready to type over */
    edit_select_all(c->name);
}

// Extract

typedef struct {
    window *win, *owner;
    char zip[FS_PATH_SZ];
    control *dir, *replace;
    fo_done_cb done;
    void *user;
} xz_t;

static void xz_browse_got(void *user, const char *path) {
    xz_t *x = (xz_t *)user;
    if (path)
        ctl_set_text(x->dir, path);
}

static void xz_browse(control *b, int ev) {
    (void)ev;
    xz_t *x = (xz_t *)b->user;
    char start[FS_PATH_SZ];
    parent_of(ctl_text(x->dir), start, sizeof(start));
    dlg_choose_folder(x->win, "Extract Into", start, xz_browse_got, x);
}

static void xz_cancel(control *b, int ev) {
    (void)ev;
    dlg_form_close(((xz_t *)b->user)->win);
}

static void xz_ok(control *b, int ev) {
    (void)ev;
    xz_t *x = (xz_t *)b->user;
    char dst[FS_PATH_SZ], zip[FS_PATH_SZ];
    snprintf(dst, sizeof(dst), "%s", ctl_text(x->dir));
    int n = (int)strlen(dst);
    while (n > 1 && (dst[n - 1] == '/' || dst[n - 1] == ' '))
        dst[--n] = 0;
    if (!n || dst[0] != '/') {
        dlg_message(x->win, "Extract",
                    "Give the full path of a folder, like /HDA/DOCS.");
        return;
    }
    bool is_dir;
    if (fo_stat(dst, &is_dir, NULL) && !is_dir) {
        dlg_message(x->win, "Extract", "There is a file with that name.");
        return;
    }
    char parent[FS_PATH_SZ];
    parent_of(dst, parent, sizeof(parent));
    uint8_t drv;
    uint16_t cl;
    if (!fs_resolve_dir(parent, &drv, &cl)) {
        dlg_message(x->win, "Extract",
                    "The folder that should hold it does not exist.");
        return;
    }
    bool replace = ctl_value(x->replace) != 0;
    snprintf(zip, sizeof(zip), "%s", x->zip);
    window *owner = x->owner;
    fo_done_cb done = x->done;
    void *user = x->user;
    dlg_form_close(x->win);

    wd_busy();
    prog_t p = {"Extracting", 0, false};
    zip_stats_t st;
    const char *err = zip_extract(zip, dst, replace, progress, &p, &st);
    wm_invalidate_all();
    fo_changed(parent);
    fo_changed(dst);
    char msg[FS_PATH_SZ + 200];
    int l = snprintf(msg, sizeof(msg), "%s%d file%s extracted into\n%s",
                     err ? "Stopped: " : "", st.files, st.files == 1 ? "" : "s",
                     dst);
    if (err)
        l += snprintf(msg + l, sizeof(msg) - l, "\n%s.", err);
    if (st.skipped)
        l += snprintf(msg + l, sizeof(msg) - l,
                      "\n%d already there (left as they were).", st.skipped);
    if (st.unsupported)
        l += snprintf(msg + l, sizeof(msg) - l,
                      "\n%d skipped: encrypted or packed in a way ASMOS "
                      "can't read.",
                      st.unsupported);
    if (st.bad)
        l += snprintf(msg + l, sizeof(msg) - l,
                      "\n%d damaged (CRC error): not written.", st.bad);
    dlg_message(owner, "Extract", msg);
    if (!err && done)
        done(user);
}

void fo_ask_extract(window *owner, const char *zip, fo_done_cb done,
                    void *user) {
    if (!zip_is_zip(zip)) {
        dlg_message(owner, "Extract",
                    "This is not a ZIP archive (or it is damaged).");
        return;
    }
    window *w = dlg_form(owner, "Extract", 230, 88, sizeof(xz_t));
    if (!w)
        return;
    xz_t *x = (xz_t *)dlg_form_state(w);
    x->win = w;
    x->owner = owner;
    x->done = done;
    x->user = user;
    snprintf(x->zip, sizeof(x->zip), "%s", zip);
    /* into a folder named after it, next to it */
    char nm[FS_NAME_SZ], dst[FS_PATH_SZ], dir[FS_PATH_SZ];
    if (!fs_name_of(zip, nm))
        snprintf(nm, sizeof(nm), "%s", fo_base(zip));
    char head[FS_NAME_SZ + 16];
    snprintf(head, sizeof(head), "Take apart \"%s\"", nm);
    char *dot = strrchr(nm, '.');
    if (dot && dot != nm)
        *dot = 0;
    parent_of(zip, dir, sizeof(dir));
    if (fo_in_trash(zip) || fo_is_trash(dir))
        snprintf(dir, sizeof(dir), "/DESKTOP");
    fo_join(dir, nm, dst, sizeof(dst));
    ctl_label(w, 8, 5, 214, 10, head, 0);
    ctl_label(w, 8, 20, 48, 10, "Into:", 0);
    x->dir = ctl_edit(w, 40, 18, 136, 12, dst, FS_PATH_MAX, ES_FILENAME, NULL);
    control *br = ctl_button(w, 180, 18, 42, 12, "Browse", 3, xz_browse);
    br->user = x;
    x->replace = ctl_check(w, 8, 36, 214,
                           "Replace files that are already there", false, NULL);
    control *ok = ctl_button(w, 116, 56, 52, 13, "Extract", 1, xz_ok);
    control *ca = ctl_button(w, 172, 56, 50, 13, "Cancel", 2, xz_cancel);
    ok->user = ca->user = x;
    ctl_style_add(ok, CS_DEFAULT);
    ctl_style_add(ca, CS_CANCEL);
    ctl_focus(ok);
}
