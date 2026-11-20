/* Help pages: the folders in /HELP, the pages an .APP carries, the Help
 * menu (os/help.h). */
#include "os/help.h"
#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "os/app_registry.h"
#include "os/appload.h"
#include "os/prefs.h"
#include "ui/ui.h"

#define HELP_PREFS "HELP" /* /PREFS/HELP.INI: which .APP's pages were put */
#define TRAILER 12        /* count, size, "ASHP" at the end of an .APP */
#define HELP_MAX (64u * 1024u)

void help_folder_of(const char *app, char *out, int sz) {
    int k = 0;
    for (const char *p = app; *p && k < sz - 1 && k < 40; p++)
        out[k++] = fs_new_name_char_ok(*p) && *p != '.' ? *p : '_';
    out[k] = 0;
    if (!k)
        snprintf(out, sz, "App");
}

static void folder_path(const char *app, char *out, int sz) {
    char f[48];
    help_folder_of(app, f, sizeof(f));
    snprintf(out, sz, "%s/%s", HELP_DIR, f);
}

static bool any_cb(const dir_entry_t *e, const char *name, void *user) {
    if (name[0] != '.' && !(e->attr & (ATTR_DIRECTORY | ATTR_VOLUME_ID)))
        *(bool *)user = true;
    return !*(bool *)user;
}

bool help_has(const char *app) {
    char p[FS_PATH_SZ];
    folder_path(app, p, sizeof(p));
    uint8_t drive;
    uint16_t cl;
    if (!fs_resolve_dir(p, &drive, &cl))
        return false;
    bool any = false;
    fs_walk_dir(drive, cl, any_cb, &any);
    return any;
}

const char *help_app_name(const app_descriptor *d) {
    for (int i = 0; i < app_registry_count; i++)
        if (app_registry[i].desc == d)
            return app_registry[i].menu_label;
    return d && d->name ? d->name : "";
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

/* the pages at the end of an .APP: name length, name, text length, text;
 * then the count, their size and "ASHP" */
static void install_from_app(const char *app, const char *apath) {
    fat_file_t f;
    if (!fs_open(apath, &f))
        return;
    char sig[40], seen[40];
    snprintf(sig, sizeof(sig), "%u,%u,%u", f.entry.file_size,
             f.entry.modify_date, f.entry.modify_time);
    char key[24];
    help_folder_of(app, key, sizeof(key));
    if (app_prefs_get(HELP_PREFS, key, seen, sizeof(seen)) &&
        !strcmp(seen, sig)) {
        fs_close(&f); /* this very file: put once already */
        return;
    }
    uint8_t t[TRAILER];
    uint32_t size = f.entry.file_size, count = 0, blob = 0;
    bool ok = size > TRAILER && fs_seek(&f, size - TRAILER) &&
              fs_read(&f, t, TRAILER) == TRAILER && !memcmp(t + 8, "ASHP", 4);
    if (ok) {
        count = le32(t), blob = le32(t + 4);
        ok = blob && blob <= HELP_MAX && blob <= size - TRAILER &&
             fs_seek(&f, size - TRAILER - blob);
    }
    uint8_t *d = ok ? kmalloc(blob) : NULL;
    ok = d && fs_read(&f, d, (int)blob) == (int)blob;
    fs_close(&f);
    app_prefs_set(HELP_PREFS, key, sig); /* (with pages or not: looked at) */
    if (!ok) {
        kfree(d);
        return;
    }
    char dir[FS_PATH_SZ];
    folder_path(app, dir, sizeof(dir));
    dir_entry_t de;
    if (!fs_find(HELP_DIR, &de))
        fs_mkdir(HELP_DIR);
    if (!fs_find(dir, &de))
        fs_mkdir(dir);
    uint32_t p = 0;
    for (uint32_t i = 0; i < count && p < blob; i++) {
        uint32_t nl = d[p++];
        if (!nl || p + nl + 4 > blob)
            break;
        char name[FS_NAME_SZ];
        uint32_t k = 0;
        for (; k < nl && k < FS_NAME_MAX; k++)
            name[k] =
                fs_new_name_char_ok((char)d[p + k]) ? (char)d[p + k] : '_';
        name[k] = 0;
        p += nl;
        uint32_t len = le32(d + p);
        p += 4;
        if (p + len > blob)
            break;
        char path[FS_PATH_SZ];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (fs_find(path, &de))
            fs_delete(path); /* (the .APP's newer page) */
        fat_file_t o;
        if (fs_create(path, &o)) {
            fs_write(&o, d + p, (int)len);
            fs_close(&o);
        }
        p += len;
    }
    kfree(d);
}

/* Help > View Help: the app whose window is in front */
static void view_help(void) {
    window *w = wm_focused_window();
    app_instance_t *a = os_instance_of_window(w);
    help_open_app(a ? help_app_name(a->desc) : "ASMOS");
}

void help_attach(app_instance_t *a) {
    if (!a || !a->desc)
        return;
    const char *name = help_app_name(a->desc);
    const char *apath = appload_path_of(a->desc);
    if (apath) /* an .APP: its own pages, the first time */
        install_from_app(name, apath);
    window *w = os_instance_window(a);
    if (!w || !help_has(name))
        return;
    menu *m = NULL;
    for (int i = 0; i < w->win_menu_count && !m; i++)
        if (!strcmp(w->win_menus[i].title, "Help"))
            m = &w->win_menus[i]; /* it has one: View Help goes in it */
    if (m) {
        menu_add_separator(m);
    } else {
        m = window_add_menu(w, "Help");
        if (!m)
            return;
    }
    menu_add_item(m, "View Help", view_help);
}

void help_open_app(const char *app) {
    char p[FS_PATH_SZ];
    folder_path(app, p, sizeof(p));
    help_open(p);
}
