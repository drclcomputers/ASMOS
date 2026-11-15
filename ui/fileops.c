#include "ui/fileops.h"
#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/wav.h"
#include "os/appload.h"
#include "os/clipboard.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/watchdog.h"
#include "ui/desktop_fs.h"
#include "ui/dialogs.h"

/* openers living in the apps */
extern void ff_open_dir_pub(uint16_t cluster, const char *path);
extern void ff_dir_changed_pub(const char *dir);
extern void teditor_open_file(const char *path);
extern void hexview_open_file(const char *path);
extern void asmdraw_open_file(const char *path);

/* ── paths ─────────────────────────────────────────────────────────────── */

void fo_join(const char *dir, const char *name, char *out, int sz) {
    int n = (int)strlen(dir);
    if (n && dir[n - 1] == '/')
        snprintf(out, sz, "%s%s", dir, name);
    else
        snprintf(out, sz, "%s/%s", dir, name);
}

const char *fo_base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void fo_parent(const char *path, char *out, int sz) {
    strncpy(out, path, sz - 1);
    out[sz - 1] = 0;
    char *s = strrchr(out, '/');
    if (s && s != out)
        *s = 0;
    else
        strcpy(out, "/");
}

static bool is_mount(const char *comp) {
    for (int m = 0; m < VFS_MOUNT_COUNT; m++)
        if (!strcasecmp(comp, g_vfs_mounts[m].path + 1))
            return true;
    return false;
}

void fo_norm(const char *path, char *out, int sz) {
    const char *p = path;
    /* "/HDA" or "/HDA/..." -> HDA is the default drive */
    if ((p[0] == '/') && (p[1] | 32) == 'h' && (p[2] | 32) == 'd' &&
        (p[3] | 32) == 'a' && (p[4] == '/' || p[4] == 0))
        p += 4;
    int n = 0;
    if (*p != '/' && n < sz - 1)
        out[n++] = '/';
    for (; *p && n < sz - 1; p++)
        out[n++] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
    while (n > 1 && out[n - 1] == '/')
        n--;
    out[n] = 0;
}

void fo_vpath(const char *path, char *out, int sz) {
    char n[270];
    fo_norm(path, n, sizeof(n));
    char first[16] = "";
    int i = 0;
    for (const char *p = n + 1; *p && *p != '/' && i < 15; p++)
        first[i++] = *p;
    first[i] = 0;
    if (is_mount(first))
        snprintf(out, sz, "%s", n);
    else if (!strcmp(n, "/"))
        snprintf(out, sz, "/HDA");
    else
        snprintf(out, sz, "/HDA%s", n);
}

uint8_t fo_drive(const char *path) {
    char n[270];
    fo_norm(path, n, sizeof(n));
    for (int m = 0; m < VFS_MOUNT_COUNT; m++) {
        int l = (int)strlen(g_vfs_mounts[m].path);
        if (!strncasecmp(n, g_vfs_mounts[m].path, l) &&
            (n[l] == '/' || n[l] == 0))
            return g_vfs_mounts[m].drive_id;
    }
    return DRIVE_HDA;
}

bool fo_stat(const char *path, bool *is_dir, uint32_t *size) {
    dir_entry_t de;
    if (!fs_find(path, &de))
        return false;
    if (is_dir)
        *is_dir = (de.attr & ATTR_DIRECTORY) != 0;
    if (size)
        *size = de.file_size;
    return true;
}

bool fo_protected(const char *path) {
    char n[270];
    fo_norm(path, n, sizeof(n));
    return !strcmp(n, "/") || path_is_protected(fo_base(n)) ||
           path_is_protected(n);
}

/* Drive + directory cluster for a folder path (drive roots are cluster 0). */
static bool dir_of(const char *path, uint8_t *drive, uint16_t *cluster) {
    char n[270];
    fo_norm(path, n, sizeof(n));
    *drive = fo_drive(n);
    if (!fs_drive_mounted(*drive))
        return false;
    if (!strcmp(n, "/")) {
        *cluster = 0;
        return true;
    }
    for (int m = 0; m < VFS_MOUNT_COUNT; m++)
        if (!strcasecmp(n, g_vfs_mounts[m].path)) {
            *cluster = 0;
            return true;
        }
    dir_entry_t de;
    if (!fs_find(path, &de) || !(de.attr & ATTR_DIRECTORY))
        return false;
    *cluster = de.cluster_lo;
    return true;
}

int fo_list_dirs(const char *dir, char (*names)[13], int max, bool hidden) {
    uint8_t drive;
    uint16_t cl;
    if (!dir_of(dir, &drive, &cl))
        return 0;
    static dir_entry_t raw[128];
    int count = 0, n = 0;
    fs_list_dir(drive, cl, raw, 128, &count);
    for (int i = 0; i < count && n < max; i++) {
        if (!(raw[i].attr & ATTR_DIRECTORY) || raw[i].name[0] == '.')
            continue;
        if ((raw[i].attr & ATTR_HIDDEN) && !hidden)
            continue;
        int k = 0;
        for (int j = 0; j < 8 && raw[i].name[j] != ' '; j++)
            names[n][k++] = raw[i].name[j];
        if (raw[i].ext[0] != ' ') {
            names[n][k++] = '.';
            for (int j = 0; j < 3 && raw[i].ext[j] != ' '; j++)
                names[n][k++] = raw[i].ext[j];
        }
        names[n][k] = 0;
        n++;
    }
    for (int i = 1; i < n; i++) /* insertion sort by name */
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[13];
            memcpy(t, names[j], 13);
            memcpy(names[j], names[j - 1], 13);
            memcpy(names[j - 1], t, 13);
        }
    return n;
}

/* ── change notification ───────────────────────────────────────────────── */

void fo_changed(const char *dir) {
    char n[270];
    fo_norm(dir, n, sizeof(n));
    if (!strcmp(n, DESKTOP_PATH) || !strncmp(n, DESKTOP_PATH "/", 9) ||
        !strcmp(n, "/"))
        desktop_fs_set_dirty();
    ff_dir_changed_pub(dir);
}

static void changed_at(const char *path) {
    char d[270];
    fo_parent(path, d, sizeof(d));
    fo_changed(d);
}

static void forget_origin(const char *name);

/* ── operations ────────────────────────────────────────────────────────── */

const char *fo_create(const char *dir, const char *name, bool is_dir,
                      bool hidden) {
    const char *err = NULL;
    if (fo_is_trash(dir) || fo_in_trash(dir))
        return "nothing can be made in the Trash";
    if (!fs_validate_name(name, is_dir, &err))
        return err ? err : "invalid name";
    char path[270];
    fo_join(dir, name, path, sizeof(path));
    if (fo_stat(path, NULL, NULL))
        return "an item with that name already exists";
    bool ok;
    if (is_dir) {
        ok = fs_mkdir(path);
    } else {
        fat_file_t f;
        ok = fs_create(path, &f);
        if (ok)
            fs_close(&f);
    }
    if (!ok)
        return "could not create it (disk full?)";
    if (hidden)
        fs_set_hidden(path, true);
    fo_changed(dir);
    return NULL;
}

const char *fo_rename(const char *path, const char *new_name) {
    if (fo_protected(path))
        return "this item is protected";
    bool is_dir = false;
    if (!fo_stat(path, &is_dir, NULL))
        return "item not found";
    if (!strcasecmp(fo_base(path), new_name))
        return NULL; /* unchanged */
    const char *err = NULL;
    if (!fs_validate_name(new_name, is_dir, &err))
        return err ? err : "invalid name";
    char dir[270], dst[270];
    fo_parent(path, dir, sizeof(dir));
    fo_join(dir, new_name, dst, sizeof(dst));
    if (fo_stat(dst, NULL, NULL))
        return "an item with that name already exists";
    if (!fs_rename(path, new_name))
        return "rename failed";
    fo_changed(dir);
    return NULL;
}

const char *fo_delete(const char *path) {
    if (fo_protected(path))
        return "this item is protected";
    bool is_dir = false;
    if (!fo_stat(path, &is_dir, NULL))
        return "item not found";
    if (is_dir)
        wd_busy(); /* hourglass: a folder can take a while */
    if (!(is_dir ? fs_rm_rf(path) : fs_delete(path)))
        return "delete failed";
    if (fo_in_trash(path))
        forget_origin(fo_base(path));
    changed_at(path);
    return NULL;
}

/* Copy or move `src` to `dst` (full paths). Across drives a move is a copy
 * followed by deleting the original. */
static const char *transfer(const char *src, const char *dst, bool move) {
    bool is_dir = false;
    if (!fo_stat(src, &is_dir, NULL))
        return "source not found";
    char ns[270], nd[270];
    fo_norm(src, ns, sizeof(ns));
    fo_norm(dst, nd, sizeof(nd));
    if (!strcmp(ns, nd))
        return move ? NULL : "it is already here";
    int l = (int)strlen(ns);
    if (is_dir && !strncmp(nd, ns, l) && nd[l] == '/')
        return "can't put a folder inside itself";
    if (fo_stat(dst, NULL, NULL))
        return "an item with that name already exists there";
    bool ok;
    wd_busy();
    if (move && fo_drive(src) == fo_drive(dst)) {
        ok = is_dir ? fs_move_dir(src, dst) : fs_move_file(src, dst);
    } else {
        ok = is_dir ? fs_copy_dir(src, dst) : fs_copy_file(src, dst);
        if (ok && move)
            ok = is_dir ? fs_rm_rf(src) : fs_delete(src);
    }
    if (!ok)
        return move ? "move failed (disk full?)" : "copy failed (disk full?)";
    changed_at(src);
    changed_at(dst);
    return NULL;
}

const char *fo_move(const char *src, const char *dst_dir) {
    if (fo_protected(src))
        return "this item is protected";
    if (fo_is_trash(dst_dir)) /* dropped on the Trash */
        return fo_trash(src);
    if (fo_in_trash(src) || fo_in_trash(dst_dir))
        return fo_in_trash(src) ? "use Put Back to take it out of the Trash"
                                : "the Trash takes items only as a whole";
    char dst[270];
    fo_join(dst_dir, fo_base(src), dst, sizeof(dst));
    return transfer(src, dst, true);
}

const char *fo_set_hidden(const char *path, bool hidden) {
    if (fo_protected(path))
        return "this item is protected";
    if (!fs_set_hidden(path, hidden))
        return "could not change the attribute";
    changed_at(path);
    return NULL;
}

const char *fo_clip(const char *path, bool cut) {
    if (fo_protected(path))
        return "this item is protected";
    if (fo_in_trash(path))
        return "use Put Back to take it out of the Trash";
    bool is_dir = false;
    if (!fo_stat(path, &is_dir, NULL))
        return "item not found";
    char dir[270];
    fo_parent(path, dir, sizeof(dir));
    clipboard_set_file(dir, fo_base(path), is_dir, cut, fo_drive(path));
    return NULL;
}

const char *fo_paste(const char *dst_dir) {
    if (!clipboard_has_file())
        return "the clipboard holds no file";
    if (fo_is_trash(dst_dir) || fo_in_trash(dst_dir))
        return "nothing can be pasted into the Trash";
    char src[270], dst[270];
    fo_join(g_clipboard.src_path, g_clipboard.name, src, sizeof(src));
    fo_join(dst_dir, g_clipboard.name, dst, sizeof(dst));
    bool cut = g_clipboard.is_cut;
    const char *err = transfer(src, dst, cut);
    if (!err && cut)
        clipboard_clear(); /* a cut item can be pasted once */
    return err;
}

static const char *type_name(const char *name, bool is_dir) {
    if (is_dir)
        return "Folder";
    const char *dot = strrchr(name, '.');
    const char *e = dot ? dot + 1 : "";
    static const char *const map[][2] = {
        {"TXT", "Text file"},       {"APP", "Application"}, {"PIC", "Picture"},
        {"WAV", "Sound"},           {"MID", "MIDI music"},  {"CFG", "Settings"},
        {"ASM", "Assembly source"}, {"BIN", "Binary"},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcasecmp(e, map[i][0]))
            return map[i][1];
    return *e ? "File" : "File (no extension)";
}

void fo_info_text(const char *path, char *out, int sz) {
    dir_entry_t de;
    char vp[270];
    fo_vpath(path, vp, sizeof(vp));
    if (!fs_find(path, &de)) {
        snprintf(out, sz, "Path:   %s\n(not found)", vp);
        return;
    }
    bool dir = (de.attr & ATTR_DIRECTORY) != 0;
    uint16_t d = de.modify_date, t = de.modify_time;
    char size[24];
    if (dir)
        strcpy(size, "-");
    else if (de.file_size >= 10240)
        snprintf(size, sizeof(size), "%u KB", de.file_size / 1024);
    else
        snprintf(size, sizeof(size), "%u bytes", de.file_size);
    char date[24];
    unsigned mo = (d >> 5) & 0x0F, dy = d & 0x1F;
    if (mo < 1 || mo > 12 || dy < 1)
        strcpy(date, "unknown"); /* no clock when it was written */
    else
        snprintf(date, sizeof(date), "%04u-%02u-%02u %02u:%02u",
                 ((d >> 9) & 0x7F) + 1980, mo, dy, (t >> 11) & 0x1F,
                 (t >> 5) & 0x3F);
    snprintf(out, sz,
             "Name:   %s\nPath:   %s\nType:   %s\nSize:   %s\n"
             "Date:   %s\nHidden: %s",
             fo_base(path), vp, type_name(fo_base(path), dir), size, date,
             (de.attr & ATTR_HIDDEN) ? "yes" : "no");
}

/* ── open ──────────────────────────────────────────────────────────────── */

bool fo_launch_app(window *owner, const char *path) {
    char name[16];
    strncpy(name, fo_base(path), sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    char *dot = strrchr(name, '.');
    if (dot)
        *dot = 0;

    /* 1. a real package: that file, else NAME.APP in the usual places */
    if (appload_try_open(path) || appload_try_open(name))
        return true;
    /* 2. a shortcut to a built-in app: try "NAME", "Name", "name" */
    char v[3][16];
    for (int k = 0; k < 16; k++) {
        char c = name[k];
        v[0][k] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        v[1][k] = k == 0 ? v[0][k] : (c >= 'A' && c <= 'Z') ? c + 32 : c;
        v[2][k] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    for (int n = 0; n < 3; n++) {
        app_descriptor *d = os_find_app(v[n]);
        if (d) {
            os_launch_app(d);
            return true;
        }
    }
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Could not find application '%s'.\nPut %s.APP in /apps or "
             "/FDD0, or rebuild the kernel.",
             name, name);
    dlg_message(owner, "App Not Found", msg);
    return false;
}

static bool ext_is(const char *name, const char *const *list) {
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    for (; *list; list++)
        if (!strcasecmp(dot + 1, *list))
            return true;
    return false;
}

void fo_open(window *owner, const char *path) {
    dir_entry_t de;
    if (!fs_find(path, &de)) {
        dlg_message(owner, "Open", "Item not found.");
        return;
    }
    const char *name = fo_base(path);
    if (de.attr & ATTR_DIRECTORY) {
        char vp[270];
        fo_vpath(path, vp, sizeof(vp));
        ff_open_dir_pub(de.cluster_lo, vp);
        return;
    }
    static const char *const text[] = {"TXT", "CFG", "ASM", "MD",  "INI",
                                       "BAT", "C",   "H",   "LOG", NULL};
    static const char *const app[] = {"APP", NULL};
    static const char *const wav[] = {"WAV", NULL};
    static const char *const pic[] = {"PIC", NULL};
    if (ext_is(name, app)) {
        fo_launch_app(owner, path);
    } else if (ext_is(name, text) || !strchr(name, '.')) {
        teditor_open_file(path);
    } else if (ext_is(name, wav)) {
        const char *err = wav_play_file(path, false);
        if (err)
            fo_report(owner, "Play", err);
    } else if (ext_is(name, pic)) {
        asmdraw_open_file(path);
    } else {
        hexview_open_file(path);
    }
}

/* ── dialogs ───────────────────────────────────────────────────────────── */

typedef struct {
    window *owner;
    char path[270]; /* dir for new, item for rename/delete */
    bool is_dir;
    bool hidden; /* the "Hidden" box (must outlive the dialog) */
    fo_done_cb done;
    void *user;
} ask_t;

static ask_t *ask_new(window *owner, const char *path, fo_done_cb done,
                      void *user) {
    ask_t *a = (ask_t *)kmalloc(sizeof(ask_t));
    if (!a)
        return NULL;
    memset(a, 0, sizeof(*a));
    a->owner = owner;
    strncpy(a->path, path, sizeof(a->path) - 1);
    a->done = done;
    a->user = user;
    return a;
}

void fo_report(window *owner, const char *title, const char *err) {
    if (!err)
        return;
    char m[96];
    snprintf(m, sizeof(m), "%s", err);
    if (m[0] >= 'a' && m[0] <= 'z')
        m[0] -= 32; /* sentence case */
    dlg_message(owner, title, m);
}

static void finish(ask_t *a, const char *title, const char *err) {
    if (err)
        fo_report(a->owner, title, err);
    else if (a->done)
        a->done(a->user);
    kfree(a);
}

static void new_done(void *user, const char *text) {
    ask_t *a = user;
    if (!text) {
        kfree(a);
        return;
    }
    finish(a, a->is_dir ? "New Folder" : "New File",
           fo_create(a->path, text, a->is_dir, a->hidden));
}

void fo_ask_new(window *owner, const char *dir, bool is_dir, fo_done_cb done,
                void *user) {
    ask_t *a = ask_new(owner, dir, done, user);
    if (!a)
        return;
    a->is_dir = is_dir;
    dlg_input_check(owner, is_dir ? "New Folder" : "New File",
                    is_dir ? "Folder name:" : "File name:", "", 12, ES_FILENAME,
                    "Hidden", &a->hidden, new_done, a);
}

static void rename_done(void *user, const char *text) {
    ask_t *a = user;
    if (!text) {
        kfree(a);
        return;
    }
    finish(a, "Rename", fo_rename(a->path, text));
}

void fo_ask_rename(window *owner, const char *path, fo_done_cb done,
                   void *user) {
    if (fo_protected(path)) {
        fo_report(owner, "Rename", "this item is protected");
        return;
    }
    ask_t *a = ask_new(owner, path, done, user);
    if (a)
        dlg_input(owner, "Rename", "New name:", fo_base(path), 12, ES_FILENAME,
                  rename_done, a);
}

static void delete_done(void *user, bool yes) {
    ask_t *a = user;
    if (!yes) {
        kfree(a);
        return;
    }
    finish(a, "Delete", fo_delete(a->path));
}

void fo_ask_delete(window *owner, const char *path, fo_done_cb done,
                   void *user) {
    if (fo_protected(path)) {
        fo_report(owner, "Delete", "this item is protected");
        return;
    }
    if (fo_trashable(path)) { /* undoable: no question */
        const char *err = fo_trash(path);
        fo_report(owner, "Delete", err);
        if (!err && done)
            done(user);
        return;
    }
    ask_t *a = ask_new(owner, path, done, user);
    if (!a)
        return;
    bool is_dir = false;
    fo_stat(path, &is_dir, NULL);
    char m[120];
    snprintf(m, sizeof(m),
             fo_in_trash(path) ? "Delete %s for good?\nIt can't be put back."
             : is_dir ? "Delete folder %s and everything\nin it? (no Trash "
                        "on this drive)"
                      : "Delete %s?\n(no Trash on this drive)",
             fo_base(path));
    dlg_confirm(owner, "Delete", m, delete_done, a);
}

void fo_show_info(window *owner, const char *path) {
    static char text[400];
    fo_info_text(path, text, sizeof(text));
    dlg_message(owner, "Get Info", text);
}

/* ── search ────────────────────────────────────────────────────────────── */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool glob(const char *n, const char *p) {
    for (; *p; p++, n++) {
        if (*p == '*') {
            while (p[1] == '*')
                p++;
            if (!p[1])
                return true;
            for (; *n; n++)
                if (glob(n, p + 1))
                    return true;
            return false;
        }
        if (!*n || (*p != '?' && lower(*p) != lower(*n)))
            return false;
    }
    return !*n;
}

bool fo_match(const char *name, const char *pattern) {
    if (!pattern || !pattern[0])
        return true;
    if (strchr(pattern, '*') || strchr(pattern, '?'))
        return glob(name, pattern);
    int pl = (int)strlen(pattern);
    for (const char *n = name; *n; n++) {
        int k = 0;
        while (k < pl && n[k] && lower(n[k]) == lower(pattern[k]))
            k++;
        if (k == pl)
            return true;
    }
    return false;
}

#define FIND_DEPTH 8
#define FIND_BATCH 128

typedef struct {
    const char *pattern;
    bool hidden, stop;
    fo_find_cb cb;
    void *user;
    int found;
} find_t;

static void find_in(find_t *f, const char *dir, int depth) {
    uint8_t drive;
    uint16_t cl;
    if (f->stop || !dir_of(dir, &drive, &cl))
        return;
    dir_entry_t *raw = (dir_entry_t *)kmalloc(FIND_BATCH * sizeof(dir_entry_t));
    if (!raw)
        return;
    int count = 0;
    fs_list_dir(drive, cl, raw, FIND_BATCH, &count);
    char path[270], name[13];
    for (int pass = 0; pass < 2 && !f->stop; pass++) /* files, then folders */
        for (int i = 0; i < count && !f->stop; i++) {
            const dir_entry_t *e = &raw[i];
            bool is_dir = (e->attr & ATTR_DIRECTORY) != 0;
            if (e->name[0] == '.' || (e->attr & ATTR_VOLUME_ID) ||
                ((e->attr & ATTR_HIDDEN) && !f->hidden))
                continue;
            int k = 0;
            for (int j = 0; j < 8 && e->name[j] != ' '; j++)
                name[k++] = e->name[j];
            if (e->ext[0] != ' ') {
                name[k++] = '.';
                for (int j = 0; j < 3 && e->ext[j] != ' '; j++)
                    name[k++] = e->ext[j];
            }
            name[k] = 0;
            fo_join(dir, name, path, sizeof(path));
            if (pass == 0 && fo_match(name, f->pattern)) {
                f->found++;
                if (!f->cb(path, is_dir, e->file_size, f->user))
                    f->stop = true;
            }
            if (pass == 1 && is_dir && depth < FIND_DEPTH)
                find_in(f, path, depth + 1);
        }
    kfree(raw);
}

int fo_find(const char *root, const char *pattern, bool hidden, fo_find_cb cb,
            void *user) {
    if (!cb)
        return 0;
    find_t f = {pattern, hidden, false, cb, user, 0};
    wd_busy();
    find_in(&f, root && root[0] ? root : "/", 0);
    return f.found;
}

/* ── Trash ─────────────────────────────────────────────────────────────── */

#define ORIGINS "TRASH" /* app_prefs file: trash name -> original folder */

static void forget_origin(const char *name) {
    app_prefs_set(ORIGINS, name, ""); /* empty value = no record */
}

bool fo_is_trash(const char *path) {
    char n[270];
    fo_norm(path, n, sizeof(n));
    return !strcmp(n, FO_TRASH);
}

bool fo_in_trash(const char *path) {
    char n[270];
    fo_norm(path, n, sizeof(n));
    int l = (int)strlen(FO_TRASH);
    return !strncmp(n, FO_TRASH, l) && n[l] == '/';
}

bool fo_trashable(const char *path) {
    char n[270];
    fo_norm(path, n, sizeof(n));
    return fo_drive(n) == DRIVE_HDA && !fo_is_trash(n) && !fo_in_trash(n) &&
           fs_drive_mounted(DRIVE_HDA);
}

static bool ensure_trash(void) {
    dir_entry_t de;
    if (fs_find(FO_TRASH, &de))
        return (de.attr & ATTR_DIRECTORY) != 0;
    if (!fs_mkdir(FO_TRASH))
        return false;
    fs_set_hidden(FO_TRASH, true); /* not among the root's items */
    return true;
}

/* NAME.EXT, or NAME~2.EXT ... if taken */
static bool trash_name(const char *base, char *out, int sz) {
    char stem[9], ext[4] = "";
    const char *dot = strrchr(base, '.');
    int sl = dot ? (int)(dot - base) : (int)strlen(base);
    if (sl > 8)
        sl = 8;
    memcpy(stem, base, sl);
    stem[sl] = 0;
    if (dot)
        strncpy(ext, dot + 1, 3), ext[3] = 0;
    for (int k = 1; k < 100; k++) {
        char nm[13], p[270];
        if (k == 1)
            snprintf(nm, sizeof(nm), "%s", base);
        else {
            int keep = 8 - (k < 10 ? 2 : 3); /* room for ~N or ~NN */
            char st[9];
            snprintf(st, sizeof(st), "%s", stem);
            if (keep < sl)
                st[keep] = 0;
            snprintf(nm, sizeof(nm), "%s~%d%s%s", st, k, ext[0] ? "." : "",
                     ext);
        }
        fo_join(FO_TRASH, nm, p, sizeof(p));
        if (!fo_stat(p, NULL, NULL)) {
            snprintf(out, sz, "%s", nm);
            return true;
        }
    }
    return false;
}

const char *fo_trash(const char *path) {
    if (fo_protected(path))
        return "this item is protected";
    if (!fo_trashable(path))
        return "only items on HDA go to the Trash";
    if (!fo_stat(path, NULL, NULL))
        return "item not found";
    if (!ensure_trash())
        return "could not make the Trash folder";
    char nm[13], dst[270], dir[270];
    if (!trash_name(fo_base(path), nm, sizeof(nm)))
        return "the Trash is too full: empty it";
    fo_join(FO_TRASH, nm, dst, sizeof(dst));
    const char *err = transfer(path, dst, true);
    if (err)
        return err;
    fo_parent(path, dir, sizeof(dir));
    char n[270];
    fo_norm(dir, n, sizeof(n));
    app_prefs_set(ORIGINS, nm, n);
    fo_changed(FO_TRASH);
    desktop_fs_set_dirty(); /* the Trash icon fills */
    return NULL;
}

const char *fo_put_back(const char *trash_path) {
    if (!fo_in_trash(trash_path))
        return "it is not in the Trash";
    char dir[96], dst[270];
    const char *nm = fo_base(trash_path);
    if (!app_prefs_get(ORIGINS, nm, dir, sizeof(dir)) || !dir[0])
        snprintf(dir, sizeof(dir), "%s", DESKTOP_PATH); /* unknown: desktop */
    bool is_dir;
    if (!fo_stat(dir, &is_dir, NULL) || !is_dir)
        snprintf(dir, sizeof(dir), "%s", DESKTOP_PATH); /* folder is gone */
    /* back under its own name when we renamed it (NAME~2.EXT) */
    char orig[13];
    snprintf(orig, sizeof(orig), "%s", nm);
    char *t = strchr(orig, '~');
    if (t) {
        char *dot = strchr(t, '.');
        if (dot)
            memmove(t, dot, strlen(dot) + 1);
        else
            *t = 0;
    }
    fo_join(dir, orig, dst, sizeof(dst));
    if (fo_stat(dst, NULL, NULL))
        fo_join(dir, nm, dst, sizeof(dst));
    const char *err = transfer(trash_path, dst, true);
    if (err)
        return err;
    forget_origin(nm);
    fo_changed(FO_TRASH);
    desktop_fs_set_dirty();
    return NULL;
}

int fo_trash_count(void) {
    uint8_t drive;
    uint16_t cl;
    if (!dir_of(FO_TRASH, &drive, &cl))
        return 0;
    static dir_entry_t raw[64];
    int count = 0, n = 0;
    fs_list_dir(drive, cl, raw, 64, &count);
    for (int i = 0; i < count; i++)
        if (raw[i].name[0] != '.' && !(raw[i].attr & ATTR_VOLUME_ID))
            n++;
    return n;
}

const char *fo_empty_trash(void) {
    uint8_t drive;
    uint16_t cl;
    if (!dir_of(FO_TRASH, &drive, &cl))
        return NULL; /* no Trash: nothing to do */
    wd_busy();
    for (int pass = 0; pass < 32; pass++) { /* 64 at a time */
        static dir_entry_t raw[64];
        int count = 0, done = 0;
        fs_list_dir(drive, cl, raw, 64, &count);
        for (int i = 0; i < count; i++) {
            if (raw[i].name[0] == '.' || (raw[i].attr & ATTR_VOLUME_ID))
                continue;
            char nm[13], p[270];
            int k = 0;
            for (int j = 0; j < 8 && raw[i].name[j] != ' '; j++)
                nm[k++] = raw[i].name[j];
            if (raw[i].ext[0] != ' ') {
                nm[k++] = '.';
                for (int j = 0; j < 3 && raw[i].ext[j] != ' '; j++)
                    nm[k++] = raw[i].ext[j];
            }
            nm[k] = 0;
            fo_join(FO_TRASH, nm, p, sizeof(p));
            bool ok =
                (raw[i].attr & ATTR_DIRECTORY) ? fs_rm_rf(p) : fs_delete(p);
            if (!ok)
                return "could not delete everything";
            done++;
        }
        if (!done)
            break;
    }
    fs_delete("/PREFS/TRASH.INI"); /* no more records */
    fo_changed(FO_TRASH);
    desktop_fs_set_dirty();
    return NULL;
}

static void empty_done(void *user, bool yes) {
    ask_t *a = user;
    if (!yes) {
        kfree(a);
        return;
    }
    finish(a, "Empty Trash", fo_empty_trash());
}

void fo_ask_empty_trash(window *owner, fo_done_cb done, void *user) {
    int n = fo_trash_count();
    if (!n) {
        dlg_message(owner, "Empty Trash", "The Trash is empty.");
        return;
    }
    ask_t *a = ask_new(owner, FO_TRASH, done, user);
    if (!a)
        return;
    char m[96];
    snprintf(m, sizeof(m), "Delete the %d item%s in the Trash\nfor good?", n,
             n == 1 ? "" : "s");
    dlg_confirm(owner, "Empty Trash", m, empty_done, a);
}
