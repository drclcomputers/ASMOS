/* FileF - file manager. The icon view is a custom control; toolbar, path
 * box, scrollbar and dialogs come from the toolkit. */
#include "config/runtime_config.h"
#include "lib/wav.h"
#include "os/api.h"
#include "os/clipboard.h"
#include "os/prefs.h"
#include "ui/desktop.h"
#include "ui/desktop_fs.h"
#include "ui/fileops.h"

#define FF_DEFAULT_W 300
#define TREE_W 78 /* folder tree pane */
#define FF_DEFAULT_H 160
#define FF_DEFAULT_X 40
#define FF_DEFAULT_Y 20
#define FF_CASCADE 16

#define ICON_CELL_W 44
#define ICON_CELL_H 30
#define CONTENT_TOP 6
#define CHAR_W 5
#define CHAR_H 6
#define LABEL_CHARS 8

#define MAX_ENTRIES 128
#define SORT_NAME 0
#define SORT_DATE 1
#define SORT_SIZE 2

#define MAX_HISTORY 32

/* ── Virtual root drive entries ─────────────────────────────── */
typedef struct {
    const char *vpath;
    const char *label;
    uint8_t drive_id;
    bool is_hdd;
} vdrive_t;

static const vdrive_t k_vdrives[] = {
    {"/HDA", "HDA", DRIVE_HDA, true},
    {"/HDB", "HDB", DRIVE_HDB, true},
    {"/FDD0", "FDD0", DRIVE_FDD0, false},
    {"/FDD1", "FDD1", DRIVE_FDD1, false},
};
#define VDRIVE_COUNT ((int)(sizeof(k_vdrives) / sizeof(k_vdrives[0])))

/* ── Item ────────────────────────────────────────────────────── */
typedef struct {
    dir_entry_t entry;
    char name[13];
    char label[LABEL_CHARS + 1];
    bool is_dotdot;
    bool is_vdrive;
    uint8_t vdrive_id;
    char vdrive_path[12];
    bool vdrive_hdd;
    int icon_x, icon_y;
    bool selected;
    bool dragging;
    int drag_off_x, drag_off_y;
    bool pinned;
    bool hidden;
} ff_item_t;

/* ── Instance ─────────────────────────────────────────────────── */
typedef struct {
    window *win; /* must stay first */
    control *back, *fwd, *up, *home, *desk, *path_edit, *view, *sb, *status_lbl;
    control *tree; /* folder tree pane (View > Folder Tree) */
    bool tree_shown;
    char tree_at[256]; /* path the tree selection was last synced to */

    uint16_t dir_cluster;
    char path[256];
    uint8_t drive_id;
    bool at_vroot;

    ff_item_t items[MAX_ENTRIES];
    int item_count;
    int sort;
    bool show_hidden;
    char filter[16]; /* typed in the view: only names matching it */
    int unfiltered;  /* items before the filter */

    bool newname_is_dir;
    bool newname_hidden; /* "Hidden" box of the New File/Folder dialog */
    char rename_old[13];

    int drag_idx;  /* item being dragged, -1 = none */
    int press_idx; /* item under the last mouse-down */
    int press_x, press_y;

    char status[48]; /* transient message; empty = item count */

    char info_name[13];
    char info_path[270];
    char info_type[32];
    char info_size[32];
    char info_date[32];
    char info_time[32];

    int scroll_y;
    int content_h_total;
    int last_view_w;

    char history[MAX_HISTORY][256];
    int history_index;
    int history_count;
} ff_inst_t;

app_descriptor filef_app;

static int s_cascade = 0;
static ff_inst_t *s_last_active = NULL;

static void ff_sync(ff_inst_t *s);
static void ff_reload(ff_inst_t *s);
static void tree_fill_roots(ff_inst_t *s);
static void tree_refresh_dir(ff_inst_t *s, const char *dir);
static void ff_navigate(ff_inst_t *s, uint16_t cluster, const char *path,
                        uint8_t drive_id);
static void ff_go_vroot(ff_inst_t *s);
static void ff_go_vroot_internal(ff_inst_t *s);
static void ff_navigate_internal(ff_inst_t *s, uint16_t cluster,
                                 const char *path, uint8_t drive_id);

/* ── Path/drive helpers ────────────────────────────────────────── */
static bool is_vroot_path(const char *path) {
    return path[0] == '/' && path[1] == '\0';
}

static bool resolve_vpath(const char *path, uint8_t *out_drive, char *out_sub,
                          int sub_sz) {
    for (int i = 0; i < VDRIVE_COUNT; i++) {
        const char *vp = k_vdrives[i].vpath;
        int vlen = (int)strlen(vp);
        if (strncmp(path, vp, vlen) == 0 &&
            (path[vlen] == '\0' || path[vlen] == '/')) {
            *out_drive = k_vdrives[i].drive_id;
            if (path[vlen] == '\0')
                strncpy(out_sub, "/", sub_sz);
            else
                strncpy(out_sub, path + vlen, sub_sz);
            out_sub[sub_sz - 1] = '\0';
            return true;
        }
    }
    return false;
}

static void build_vpath(uint8_t drive_id, const char *sub, char *out,
                        int out_sz) {
    const char *drv = "HDA";
    for (int i = 0; i < VDRIVE_COUNT; i++)
        if (k_vdrives[i].drive_id == drive_id) {
            drv = k_vdrives[i].label;
            break;
        }
    if (strcmp(sub, "/") == 0)
        snprintf(out, out_sz, "/%s", drv);
    else
        snprintf(out, out_sz, "/%s%s", drv, sub);
}

static bool resolve_dir(const char *path, uint8_t *out_drive,
                        uint16_t *out_cluster) {
    uint8_t drive;
    char sub[256];
    if (!resolve_vpath(path, &drive, sub, sizeof(sub)))
        return false;
    if (!fs_drive_mounted(drive))
        return false;
    *out_drive = drive;
    if (strcmp(sub, "/") == 0) {
        *out_cluster = 0;
        return true;
    }
    uint16_t cluster = 0;
    char component[256];
    const char *ptr = sub;
    while (*ptr) {
        while (*ptr == '/')
            ptr++;
        if (!*ptr)
            break;
        int ci = 0;
        while (*ptr && *ptr != '/')
            component[ci++] = *ptr++;
        component[ci] = '\0';
        char name83[12];
        fs_make_83(component, name83);
        dir_entry_t de;
        if (!fs_find_in_dir(drive, cluster, name83, &de))
            return false;
        if (!(de.attr & ATTR_DIRECTORY))
            return false;
        cluster = de.cluster_lo;
    }
    *out_cluster = cluster;
    return true;
}

/* ── History management ────────────────────────────────────────── */
static void ff_history_add(ff_inst_t *s, const char *new_path) {
    if (s->history_index >= 0 && s->history_index < MAX_HISTORY - 1) {
        if (strcmp(s->history[s->history_index], new_path) == 0)
            return;
    }
    s->history_count = s->history_index + 2;
    if (s->history_count > MAX_HISTORY)
        s->history_count = MAX_HISTORY;
    s->history_index++;
    if (s->history_index >= MAX_HISTORY) {
        memmove(s->history[0], s->history[1],
                (MAX_HISTORY - 1) * sizeof(s->history[0]));
        s->history_index = MAX_HISTORY - 1;
        s->history_count = MAX_HISTORY;
    }
    strncpy(s->history[s->history_index], new_path, 255);
    s->history[s->history_index][255] = '\0';
}

static void ff_history_go(ff_inst_t *s, int idx) {
    if (idx < 0 || idx >= s->history_count)
        return;
    s->history_index = idx;
    const char *p = s->history[idx];
    if (is_vroot_path(p)) {
        ff_go_vroot_internal(s);
    } else {
        uint8_t drv;
        uint16_t cluster;
        if (resolve_dir(p, &drv, &cluster)) {
            ff_navigate_internal(s, cluster, p, drv);
        } else {
            ff_go_vroot_internal(s);
        }
    }
}

/* ── Instance helpers ───────────────────────────────────────────── */
static ff_inst_t *inst_of(window *w) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &filef_app)
            continue;
        ff_inst_t *s = (ff_inst_t *)a->state;
        if (s->win == w)
            return s;
    }
    return NULL;
}

static ff_inst_t *active_finder(void) {
    window *fw = wm_focused_window();
    if (fw) {
        ff_inst_t *s = inst_of(fw);
        if (s) {
            s_last_active = s;
            return s;
        }
    }
    if (s_last_active) {
        for (int i = 0; i < MAX_RUNNING_APPS; i++) {
            app_instance_t *a = &running_apps[i];
            if (a->running && a->desc == &filef_app &&
                a->state == s_last_active)
                return s_last_active;
        }
        s_last_active = NULL;
    }
    return NULL;
}

/* ── Sort & Layout ──────────────────────────────────────────────── */
static int item_sort_cmp(const ff_item_t *a, const ff_item_t *b, int sort) {
    if (a->is_dotdot || a->is_vdrive)
        return -1;
    if (b->is_dotdot || b->is_vdrive)
        return 1;
    bool ad = (a->entry.attr & ATTR_DIRECTORY) != 0;
    bool bd = (b->entry.attr & ATTR_DIRECTORY) != 0;
    if (ad != bd)
        return ad ? -1 : 1;
    if (sort == SORT_SIZE)
        return (int)a->entry.file_size - (int)b->entry.file_size;
    if (sort == SORT_DATE) {
        int dc = (int)b->entry.modify_date - (int)a->entry.modify_date;
        return dc ? dc : (int)b->entry.modify_time - (int)a->entry.modify_time;
    }
    return strcmp(a->name, b->name);
}

static void ff_sort(ff_inst_t *s) {
    for (int i = 0; i < s->item_count - 1; i++)
        for (int j = i + 1; j < s->item_count; j++)
            if (item_sort_cmp(&s->items[i], &s->items[j], s->sort) > 0) {
                ff_item_t tmp = s->items[i];
                s->items[i] = s->items[j];
                s->items[j] = tmp;
            }
}

static int content_view_h(ff_inst_t *s) {
    return s->view ? s->view->ah - 4 : 100;
}

static int ff_cols(ff_inst_t *s) {
    int c = ((s->view ? s->view->aw - 4 : FF_DEFAULT_W) - 4) / ICON_CELL_W;
    return c < 1 ? 1 : c;
}

static void ff_layout(ff_inst_t *s) {
    int cols = ff_cols(s);
    int col = 0, row = 0, max_row = -1;
    for (int i = 0; i < s->item_count; i++) {
        ff_item_t *it = &s->items[i];
        if (it->pinned || it->dragging)
            continue;
        it->icon_x = 4 + col * ICON_CELL_W;
        it->icon_y = CONTENT_TOP + row * ICON_CELL_H;
        if (row > max_row)
            max_row = row;
        if (++col >= cols) {
            col = 0;
            row++;
        }
    }
    s->content_h_total =
        CONTENT_TOP + (max_row >= 0 ? (max_row + 1) * ICON_CELL_H : 0);
}

/* ── Reload ─────────────────────────────────────────────────────── */
static void entry_to_name(const dir_entry_t *e, char *out) {
    int j = 0;
    for (int k = 0; k < 8 && e->name[k] != ' '; k++)
        out[j++] = e->name[k];
    if (!(e->attr & ATTR_DIRECTORY) && e->name[0] != '.' && e->ext[0] != ' ') {
        out[j++] = '.';
        for (int k = 0; k < 3 && e->ext[k] != ' '; k++)
            out[j++] = e->ext[k];
    }
    out[j] = '\0';
}

static void ff_make_label(const char *name, char *out) {
    int i = 0;
    while (name[i] && i < LABEL_CHARS) {
        out[i] = name[i];
        i++;
    }
    out[i] = '\0';
}

static bool item_is_app(const ff_item_t *it) {
    if (it->is_dotdot || it->is_vdrive || (it->entry.attr & ATTR_DIRECTORY))
        return false;
    return it->entry.ext[0] == 'A' && it->entry.ext[1] == 'P' &&
           it->entry.ext[2] == 'P';
}

static void ff_reload(ff_inst_t *s) {
    s->item_count = 0;
    if (s->at_vroot) {
        for (int i = 0; i < VDRIVE_COUNT; i++) {
            if (!fs_drive_mounted(k_vdrives[i].drive_id))
                continue;
            if (s->item_count >= MAX_ENTRIES)
                break;
            ff_item_t *it = &s->items[s->item_count++];
            memset(it, 0, sizeof(ff_item_t));
            it->is_vdrive = true;
            it->vdrive_id = k_vdrives[i].drive_id;
            it->vdrive_hdd = k_vdrives[i].is_hdd;
            strncpy(it->vdrive_path, k_vdrives[i].vpath, 11);
            strncpy(it->name, k_vdrives[i].label, 12);
            ff_make_label(it->name, it->label);
        }
        ff_layout(s);
        s->drag_idx = -1;
        s->status[0] = '\0';
        s->scroll_y = 0;
        ff_sync(s);
        return;
    }

    {
        uint8_t dummy_drive;
        char sub[256];
        bool at_drive_root =
            resolve_vpath(s->path, &dummy_drive, sub, sizeof(sub)) &&
            strcmp(sub, "/") == 0;
        if (!at_drive_root) {
            ff_item_t *up = &s->items[s->item_count++];
            memset(up, 0, sizeof(ff_item_t));
            up->is_dotdot = true;
            strcpy(up->name, "..");
            strcpy(up->label, "..");
        }
    }

    dir_entry_t raw[MAX_ENTRIES];
    int count = 0;
    s->unfiltered = 0;
    fs_list_dir(s->drive_id, s->dir_cluster, raw, MAX_ENTRIES, &count);
    for (int i = 0; i < count && s->item_count < MAX_ENTRIES; i++) {
        if (raw[i].name[0] == '.')
            continue;
        bool is_hidden = (raw[i].attr & ATTR_HIDDEN) != 0;
        if (is_hidden && !s->show_hidden)
            continue;
        s->unfiltered++;
        if (s->filter[0]) {
            char nm[13];
            entry_to_name(&raw[i], nm);
            if (!fo_match(nm, s->filter))
                continue;
        }
        ff_item_t *it = &s->items[s->item_count++];
        memset(it, 0, sizeof(ff_item_t));
        it->entry = raw[i];
        it->hidden = is_hidden;
        entry_to_name(&raw[i], it->name);
        ff_make_label(it->name, it->label);
    }
    ff_sort(s);
    ff_layout(s);
    s->drag_idx = -1;
    s->status[0] = '\0';
    s->scroll_y = 0;
    ff_sync(s);
}

/* Drives appeared/disappeared: every window and the desktop. */
static void ff_refresh_all(void) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (a->running && a->desc == &filef_app && a->state) {
            ff_inst_t *fs = (ff_inst_t *)a->state;
            if (fs->tree_shown)
                tree_fill_roots(fs);
            ff_reload(fs);
        }
    }
    desktop_fs_set_dirty();
}

/* fileops: something in `dir` changed -> reload windows showing it */
void ff_dir_changed_pub(const char *dir) {
    char want[270], have[270];
    fo_norm(dir, want, sizeof(want));
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &filef_app || !a->state)
            continue;
        ff_inst_t *fs = (ff_inst_t *)a->state;
        if (fs->at_vroot)
            continue;
        fo_norm(fs->path, have, sizeof(have));
        if (!strcmp(want, have))
            ff_reload(fs);
        tree_refresh_dir(fs, dir); /* folder pane may show `dir` */
    }
}

/* ── Navigation (internal) ──────────────────────────────────────── */
static void ff_go_vroot_internal(ff_inst_t *s) {
    s->filter[0] = 0;
    s->at_vroot = true;
    s->dir_cluster = 0;
    s->drive_id = DRIVE_HDA;
    strcpy(s->path, "/");
    s->win->title = s->path;
    s->scroll_y = 0;
    ff_reload(s);
}

static void ff_navigate_internal(ff_inst_t *s, uint16_t cluster,
                                 const char *path, uint8_t drive_id) {
    s->filter[0] = 0;
    s->at_vroot = false;
    s->dir_cluster = cluster;
    s->drive_id = drive_id;
    strncpy(s->path, path, 255);
    s->path[255] = '\0';
    s->win->title = fo_is_trash(s->path) ? "Trash" : s->path;
    s->scroll_y = 0;
    ff_reload(s);
}

static void ff_go_vroot(ff_inst_t *s) {
    ff_history_add(s, "/");
    ff_go_vroot_internal(s);
}

static void ff_navigate(ff_inst_t *s, uint16_t cluster, const char *path,
                        uint8_t drive_id) {
    ff_history_add(s, path);
    ff_navigate_internal(s, cluster, path, drive_id);
}

void ff_open_dir_pub(uint16_t cluster, const char *path) {
    if (g_cfg.filef_single_window) {
        for (int i = 0; i < MAX_RUNNING_APPS; i++) {
            app_instance_t *a = &running_apps[i];
            if (!a->running || a->desc != &filef_app)
                continue;
            ff_inst_t *s = (ff_inst_t *)a->state;
            if (is_vroot_path(path))
                ff_go_vroot(s);
            else {
                uint8_t drv;
                char sub[256];
                if (resolve_vpath(path, &drv, sub, sizeof(sub)))
                    ff_navigate(s, cluster, path, drv);
            }
            wm_focus(s->win);
            return;
        }
    }
    app_instance_t *inst = os_launch_app(&filef_app);
    if (!inst)
        return;
    ff_inst_t *s = (ff_inst_t *)inst->state;
    if (is_vroot_path(path)) {
        ff_go_vroot(s);
    } else {
        uint8_t drv = DRIVE_HDA;
        char sub[256];
        resolve_vpath(path, &drv, sub, sizeof(sub));
        ff_navigate(s, cluster, path, drv);
    }
}

/* ── Drawing ─────────────────────────────────────────────────── */
/* base: content origin, already offset by scroll; clipped by the view */
static void ff_draw_item(ff_inst_t *s, int i, int base_x, int base_y) {
    ff_item_t *it = &s->items[i];
    int ax = base_x + it->icon_x + (ICON_CELL_W - ICON_W) / 2;
    int ay = base_y + it->icon_y;

    if (it->is_vdrive)
        it->vdrive_hdd ? draw_hdd_icon(ax, ay, it->selected)
                       : draw_floppy_icon(ax, ay, it->selected);
    else if (it->is_dotdot)
        draw_dotdot_icon(ax, ay, it->selected);
    else if (it->entry.attr & ATTR_DIRECTORY)
        draw_folder_icon(ax, ay, it->selected);
    else if (item_is_app(it))
        draw_app_icon(ax, ay, it->selected);
    else {
        char ext[4] = {0};
        for (int j = 0; j < 3; j++)
            ext[j] = it->entry.ext[j];
        for (int j = 2; j >= 0; j--) {
            if (ext[j] == ' ')
                ext[j] = '\0';
            else
                break;
        }
        if (!strcmp(ext, "PIC") || !strcmp(ext, "pic"))
            draw_pic_icon(ax, ay, it->selected);
        else if (!strcmp(ext, "TXT") || !strcmp(ext, "txt"))
            draw_txt_icon(ax, ay, it->selected);
        else if (!strcmp(ext, "CFG") || !strcmp(ext, "cfg"))
            draw_cfg_icon(ax, ay, it->selected);
        else
            draw_unknown_icon(ax, ay, it->selected);
    }

    if (it->hidden)
        draw_char(ax + ICON_W - 6, ay, 'H', DARK_GRAY, 2);

    bool is_cut =
        (!it->is_vdrive && !it->is_dotdot && clipboard_has_file() &&
         g_clipboard.is_cut && strcmp(g_clipboard.name, it->name) == 0 &&
         strcmp(g_clipboard.src_path, s->path) == 0);

    int lw = (int)strlen(it->label) * CHAR_W;
    int lx = ax + ICON_W / 2 - lw / 2;
    int ly = ay + ICON_H + 1;
    uint8_t lc = (is_cut || it->hidden) ? DARK_GRAY : BLACK;
    if (it->selected) {
        fill_rect(lx - 1, ly - 1, lw + 2, CHAR_H + 2, BLACK);
        draw_string(lx, ly, it->label, WHITE, 2);
    } else {
        draw_string(lx, ly, it->label, lc, 2);
    }
}

/* ── Hit testing ─────────────────────────────────────────────────── */
static bool in_content(ff_inst_t *s, int mx, int my) {
    control *v = s->view;
    return v && v->visible && mx >= v->ax + 2 && mx < v->ax + v->aw - 2 &&
           my >= v->ay + 2 && my < v->ay + v->ah - 2;
}

static int ff_hit(ff_inst_t *s, int mx, int my) {
    if (!in_content(s, mx, my))
        return -1;
    int wx = s->view->ax + 2, wy = s->view->ay + 2 - s->scroll_y;
    for (int i = 0; i < s->item_count; i++) {
        ff_item_t *it = &s->items[i];
        int ax = wx + it->icon_x + (ICON_CELL_W - ICON_W) / 2;
        int ay = wy + it->icon_y;
        if (mx >= ax && mx < ax + ICON_W && my >= ay &&
            my < ay + ICON_H + CHAR_H + 4)
            return i;
    }
    return -1;
}

static ff_inst_t *ff_find_target(int mx, int my, ff_inst_t *exclude);

/* For the desktop: folder shown by the FileF window under (mx, my). */
bool ff_folder_at(int mx, int my, char *out, int sz) {
    ff_inst_t *t = ff_find_target(mx, my, NULL);
    if (!t)
        return false;
    /* only if that window is the one on top at this point */
    for (int i = win_count - 1; i >= 0; i--) {
        window *w = win_stack[i];
        if (w->visible && !w->minimized && mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + w->h + MENUBAR_H) {
            if (w != t->win)
                return false;
            break;
        }
    }
    strncpy(out, t->path, sz - 1);
    out[sz - 1] = 0;
    return true;
}

static ff_inst_t *ff_find_target(int mx, int my, ff_inst_t *exclude) {
    for (int i = win_count - 1; i >= 0; i--) {
        window *w = win_stack[i];
        if (!w->visible)
            continue;
        ff_inst_t *t = inst_of(w);
        if (!t || t == exclude || t->at_vroot)
            continue;
        if (in_content(t, mx, my))
            return t;
    }
    return NULL;
}

#define FF_PUSH_CTX(s)                                                         \
    uint16_t _saved_cluster = dir_context.current_cluster;                     \
    uint8_t _saved_drive = dir_context.drive_id;                               \
    dir_context.current_cluster = (s)->dir_cluster;                            \
    dir_context.drive_id = (s)->drive_id;

#define FF_POP_CTX()                                                           \
    dir_context.current_cluster = _saved_cluster;                              \
    dir_context.drive_id = _saved_drive;

static void item_fspath(ff_inst_t *s, const char *name, char *out, int sz) {
    if (s->at_vroot) {
        snprintf(out, sz, "/%s", name);
        return;
    }

    uint8_t drv;
    char sub[256];
    if (!resolve_vpath(s->path, &drv, sub, sizeof(sub))) {
        snprintf(out, sz, "%s/%s", s->path, name);
        return;
    }

    if (strcmp(sub, "/") == 0) {
        build_vpath(drv, "/", out, sz);
        int len = (int)strlen(out);
        if (len >= sz - 2)
            return;
        out[len] = '/';
        out[len + 1] = '\0';
        strncpy(out + len + 1, name, sz - len - 2);
        out[sz - 1] = '\0';
    } else {
        build_vpath(drv, sub, out, sz);
        int len = (int)strlen(out);
        if (len >= sz - 2)
            return;
        out[len] = '/';
        out[len + 1] = '\0';
        strncpy(out + len + 1, name, sz - len - 2);
        out[sz - 1] = '\0';
    }
}

/* Index of the first selected real entry (not "..", not a drive), or -1. */
static int ff_selected_file(const ff_inst_t *s) {
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot &&
            !s->items[i].is_vdrive)
            return i;
    return -1;
}

static int ff_selected(const ff_inst_t *s) {
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected)
            return i;
    return -1;
}

static void ff_select(ff_inst_t *s, int idx) {
    for (int i = 0; i < s->item_count; i++)
        s->items[i].selected = (i == idx);
    if (idx < 0)
        return;
    /* keep it visible */
    int top = s->items[idx].icon_y, bot = top + ICON_CELL_H,
        vh = content_view_h(s);
    if (top < s->scroll_y)
        s->scroll_y = top - CONTENT_TOP;
    else if (bot > s->scroll_y + vh)
        s->scroll_y = bot - vh;
    if (s->scroll_y < 0)
        s->scroll_y = 0;
}

static void tree_sync(ff_inst_t *s);

/* Status, path box, nav buttons, scrollbar and folder tree follow the
 * state. */
static void ff_sync(ff_inst_t *s) {
    if (!s->view)
        return;
    int ms = s->content_h_total - content_view_h(s);
    if (ms < 0)
        ms = 0;
    if (s->scroll_y > ms)
        s->scroll_y = ms;
    ctl_set_range(s->sb, 0, ms, content_view_h(s));
    ctl_set_value(s->sb, s->scroll_y);
    ctl_enable(s->back, s->history_index > 0);
    ctl_enable(s->fwd, s->history_index < s->history_count - 1);
    ctl_enable(s->up, !s->at_vroot);
    if (s->win->focus != s->path_edit &&
        strcmp(ctl_text(s->path_edit), s->path))
        ctl_set_text(s->path_edit, s->path);
    char stat[64];
    if (s->status[0]) {
        strncpy(stat, s->status, 63);
        stat[63] = 0;
    } else {
        int real = s->item_count;
        if (!s->at_vroot && real && s->items[0].is_dotdot)
            real--;
        if (s->filter[0])
            snprintf(stat, sizeof(stat), "%d of %d match \"%s\"\tEsc: all",
                     real, s->unfiltered, s->filter);
        else
            snprintf(stat, sizeof(stat), "%d item%s%s", real,
                     real == 1 ? "" : "s",
                     s->show_hidden ? " (hidden shown)" : "");
    }
    if (strcmp(ctl_text(s->status_lbl), stat))
        ctl_set_text(s->status_lbl, stat);
    tree_sync(s);
}

static void ff_open_item(ff_inst_t *s, int idx);

static void ff_go_up(ff_inst_t *s) {
    if (s->at_vroot)
        return;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].is_dotdot) {
            ff_open_item(s, i);
            return;
        }
    ff_go_vroot(s); /* at a drive root */
}

static void ff_go_desktop(ff_inst_t *s) {
    char dp[270];
    uint8_t drv;
    uint16_t cl;
    build_vpath(DRIVE_HDA, "/DESKTOP", dp, sizeof(dp));
    if (resolve_dir(dp, &drv, &cl))
        ff_navigate(s, cl, dp, drv);
    else
        ff_go_vroot(s);
}

/* ── File operations: all through ui/fileops (shared with the desktop) ─ */

/* Full path of the selected real item; false (and a status) if none. */
static bool sel_path(ff_inst_t *s, char *out, int sz) {
    int sel = s->at_vroot ? -1 : ff_selected_file(s);
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
        return false;
    }
    item_fspath(s, s->items[sel].name, out, sz);
    return true;
}

static void op_done(void *user) {
    ff_inst_t *s = user;
    strcpy(s->status, "Done.");
}

static void new_item(bool dir) {
    ff_inst_t *s = active_finder();
    if (s && !s->at_vroot)
        fo_ask_new(s->win, s->path, dir, op_done, s);
}
static void menu_new_file(void) { new_item(false); }
static void menu_new_folder(void) { new_item(true); }

static void menu_rename(void) {
    ff_inst_t *s = active_finder();
    char p[270];
    if (s && sel_path(s, p, sizeof(p)))
        fo_ask_rename(s->win, p, op_done, s);
}

static void menu_delete(void) {
    ff_inst_t *s = active_finder();
    char p[270];
    if (s && sel_path(s, p, sizeof(p)))
        fo_ask_delete(s->win, p, op_done, s);
}

static void menu_get_info(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    int sel = ff_selected(s);
    if (sel < 0 || s->items[sel].is_dotdot) {
        strcpy(s->status, "Nothing selected.");
        return;
    }
    char p[270];
    if (s->items[sel].is_vdrive)
        strncpy(p, s->items[sel].vdrive_path, sizeof(p) - 1);
    else
        item_fspath(s, s->items[sel].name, p, sizeof(p));
    fo_show_info(s->win, p);
}

static void menu_reload(void) {
    ff_inst_t *s = active_finder();
    if (s)
        ff_reload(s);
}

static void menu_toggle_hidden(void) {
    ff_inst_t *s = active_finder();
    char p[270];
    if (!s || !sel_path(s, p, sizeof(p)))
        return;
    bool hide = !s->items[ff_selected_file(s)].hidden;
    const char *err = fo_set_hidden(p, hide);
    fo_report(s->win, "Hidden", err);
    if (!err)
        strcpy(s->status, hide ? "Marked hidden." : "Unmarked.");
}

static void menu_show_hidden_toggle(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    s->show_hidden = !s->show_hidden;
    app_prefs_set_int("FILEF", "hidden", s->show_hidden);
    ff_reload(s);
}

static void menu_copy_path(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    clipboard_set_text(s->path, (int)strlen(s->path));
    strcpy(s->status, "Path copied.");
}

static void ff_clip_selected(bool is_cut) {
    ff_inst_t *s = active_finder();
    char p[270];
    if (!s || !sel_path(s, p, sizeof(p)))
        return;
    const char *err = fo_clip(p, is_cut);
    fo_report(s->win, is_cut ? "Cut" : "Copy", err);
    if (!err)
        strcpy(s->status, is_cut ? "Cut." : "Copied.");
}
static void menu_copy(void) { ff_clip_selected(false); }
static void menu_cut(void) { ff_clip_selected(true); }

static void menu_paste(void) {
    ff_inst_t *s = active_finder();
    if (!s || s->at_vroot)
        return;
    const char *err = fo_paste(s->path);
    fo_report(s->win, "Paste", err);
    if (!err)
        strcpy(s->status, "Pasted.");
}

static void set_sort(int sort) {
    ff_inst_t *s = active_finder();
    if (s) {
        s->sort = sort;
        ff_sort(s);
        ff_layout(s);
    }
}
static void menu_sort_name(void) { set_sort(SORT_NAME); }
static void menu_sort_date(void) { set_sort(SORT_DATE); }
static void menu_sort_size(void) { set_sort(SORT_SIZE); }

static void menu_about_filef(void) {
    dlg_message(NULL, "About FileF",
                "FileF v3.0 - ASMOS File Manager\n"
                "Enter open, Backspace up, F2 rename, Del delete.\n"
                "(c)2026 @drclcomputers");
}
static void menu_close(void) {
    ff_inst_t *s = active_finder();
    if (s)
        os_close_own_instance(s->win);
}

static void menu_eject_fdd(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && s->items[i].is_vdrive &&
            !s->items[i].vdrive_hdd) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Select a floppy first.");
        return;
    }
    if (fdd_eject(s->items[sel].vdrive_id)) {
        ff_refresh_all();
        strcpy(s->status, "Floppy ejected.");
    } else {
        strcpy(s->status, "Eject failed.");
    }
}

static void menu_insert_fdd(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    if (fdd_insert(DRIVE_FDD0) || fdd_insert(DRIVE_FDD1)) {
        ff_refresh_all();
        strcpy(s->status, "Floppy mounted.");
    } else {
        dlg_message(s->win, "Insert Floppy",
                    "No floppy found or already mounted.");
    }
}

static bool over_desktop(int mx, int my) {
    if (my < MENUBAR_H || my >= SCREEN_HEIGHT - TASKBAR_H)
        return false;
    for (int i = 0; i < win_count; i++) {
        window *w = win_stack[i];
        if (!w->visible || w->minimized || w->pinned_bottom)
            continue;
        int wy = w->y + MENUBAR_H;
        if (mx >= w->x && mx < w->x + w->w && my >= wy && my < wy + w->h)
            return false;
    }
    return true;
}

/* Drop item `idx` of `src` into folder `dst_dir` (another FileF window or
 * the desktop). */
static void ff_drop_into(ff_inst_t *src, int idx, const char *dst_dir) {
    ff_item_t *it = &src->items[idx];
    if (it->is_dotdot || it->is_vdrive)
        return;
    char p[270];
    item_fspath(src, it->name, p, sizeof(p));
    const char *err = fo_move(p, dst_dir);
    fo_report(src->win, "Move", err);
    if (!err)
        strcpy(src->status, "Moved.");
}

static void ff_open_item(ff_inst_t *s, int idx) {
    ff_item_t *it = &s->items[idx];
    if (it->is_vdrive) {
        if (fs_drive_mounted(it->vdrive_id)) {
            ff_navigate(s, 0, it->vdrive_path, it->vdrive_id);
        } else {
            dlg_message(s->win, "Drive", "Drive not mounted.");
        }
    } else if (it->is_dotdot) {
        uint8_t drv;
        char sub[256];
        bool has_vp = resolve_vpath(s->path, &drv, sub, sizeof(sub));
        if (has_vp && strcmp(sub, "/") == 0) {
            ff_go_vroot(s);
        } else {
            char parent_logical[256];
            char *sl = strrchr(s->path, '/');
            if (sl && sl != s->path) {
                int plen = (int)(sl - s->path);
                strncpy(parent_logical, s->path, plen);
                parent_logical[plen] = '\0';
            } else
                strcpy(parent_logical, "/");
            uint8_t pdrv;
            uint16_t pcluster;
            if (!is_vroot_path(parent_logical) &&
                resolve_dir(parent_logical, &pdrv, &pcluster))
                ff_navigate(s, pcluster, parent_logical, pdrv);
            else
                ff_go_vroot(s);
        }
    } else if (it->entry.attr & ATTR_DIRECTORY) {
        char child_logical[270];
        snprintf(child_logical, sizeof(child_logical), "%s/%s", s->path,
                 it->name);
        uint16_t child_cluster = it->entry.cluster_lo;
        if (g_cfg.filef_single_window)
            ff_navigate(s, child_cluster, child_logical, s->drive_id);
        else
            ff_open_dir_pub(child_cluster, child_logical);
    } else {
        /* files and apps: same rules as the desktop */
        char fspath[270];
        item_fspath(s, it->name, fspath, sizeof(fspath));
        fo_open(s->win, fspath);
    }
}

/* ── Icon view control ─────────────────────────────────────────────── */
static void paint_view(control *c, int x, int y, int w, int h) {
    ff_inst_t *s = c->user;
    (void)h;
    if (w != s->last_view_w) { /* window resized */
        s->last_view_w = w;
        ff_layout(s);
        ff_sync(s);
    }
    for (int i = 0; i < s->item_count; i++)
        if (!s->items[i].dragging)
            ff_draw_item(s, i, x, y - s->scroll_y);
}

/* Dragged icon follows the pointer over every window and the desktop. */
static void draw_drag_ghost(void *user) {
    ff_inst_t *s = user;
    if (s->drag_idx < 0)
        return;
    ff_item_t *it = &s->items[s->drag_idx];
    int svx = it->icon_x, svy = it->icon_y;
    it->icon_x = mouse.x - ICON_CELL_W / 2;
    it->icon_y = mouse.y - ICON_H / 2;
    ff_draw_item(s, s->drag_idx, 0, 0);
    it->icon_x = svx;
    it->icon_y = svy;
}

static void view_drop(ff_inst_t *s) {
    int idx = s->drag_idx;
    s->drag_idx = -1;
    wm_clear_overlay(s);
    if (!in_content(s, mouse.x, mouse.y)) {
        ff_inst_t *dst = ff_find_target(mouse.x, mouse.y, s);
        if (dst) {
            s->items[idx].dragging = false;
            ff_drop_into(s, idx, dst->path); /* both windows reload */
            return;
        }
        if (over_desktop(mouse.x, mouse.y)) {
            s->items[idx].dragging = false;
            ff_drop_into(s, idx, DESKTOP_PATH);
            return;
        }
    }
    s->items[idx].dragging = false;
    ff_layout(s);
}

/* Typing filters the icons: letters add, Backspace removes, Esc clears. */
static void set_filter(ff_inst_t *s, const char *f) {
    strncpy(s->filter, f, sizeof(s->filter) - 1);
    s->filter[sizeof(s->filter) - 1] = 0;
    ff_reload(s);
    int first = (s->item_count > 0 && s->items[0].is_dotdot) ? 1 : 0;
    ff_select(s, s->item_count > first ? first : -1);
}

static bool filter_key(ff_inst_t *s) {
    uint8_t sc = kb.last_scancode;
    char c = kb.last_char;
    int n = (int)strlen(s->filter);
    if (s->at_vroot || kb.ctrl)
        return false;
    if (sc == ESC && n) {
        set_filter(s, "");
        return true;
    }
    if (sc == BACKSPACE && n) {
        char f[16];
        strcpy(f, s->filter);
        f[n - 1] = 0;
        set_filter(s, f);
        return true;
    }
    bool name_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                     c == '-' || c == '*' || c == '?' || c == '~';
    if (name_char && n < (int)sizeof(s->filter) - 1 && sc != ENTER) {
        char f[16];
        strcpy(f, s->filter);
        f[n] = (char)toupper((unsigned char)c);
        f[n + 1] = 0;
        set_filter(s, f);
        return true;
    }
    return false;
}

static void view_key(ff_inst_t *s) {
    uint8_t sc = kb.last_scancode;
    int sel = ff_selected(s), n = s->item_count, cols = ff_cols(s);
    int vh = content_view_h(s);
    if (filter_key(s))
        return;
    if (kb.ctrl_c)
        menu_copy();
    else if (kb.ctrl_x)
        menu_cut();
    else if (kb.ctrl_v)
        menu_paste();
    else if (sc == DELETE)
        menu_delete();
    else if (sc == F2)
        menu_rename();
    else if (sc == ENTER && sel >= 0)
        ff_open_item(s, sel);
    else if (sc == BACKSPACE)
        ff_go_up(s);
    else if (sc == PAGE_UP || sc == PAGE_DOWN) {
        s->scroll_y += sc == PAGE_UP ? -vh : vh;
        if (s->scroll_y < 0)
            s->scroll_y = 0;
    } else if (n > 0 &&
               (sc == LEFT_ARROW || sc == RIGHT_ARROW || sc == UP_ARROW ||
                sc == DOWN_ARROW || sc == HOME || sc == END)) {
        int t = sel < 0             ? 0
                : sc == LEFT_ARROW  ? sel - 1
                : sc == RIGHT_ARROW ? sel + 1
                : sc == UP_ARROW    ? sel - cols
                : sc == DOWN_ARROW  ? sel + cols
                : sc == HOME        ? 0
                                    : n - 1;
        ff_select(s, t < 0 ? 0 : t >= n ? n - 1 : t);
    }
}

/* ── Context menus ────────────────────────────────────────────────── */

static void ctx_open(void) {
    ff_inst_t *s = active_finder();
    int sel = s ? ff_selected(s) : -1;
    if (sel >= 0)
        ff_open_item(s, sel);
}

/* Paste into the folder that was right-clicked, else the current one. */
static void ctx_paste_into(void) {
    ff_inst_t *s = active_finder();
    char p[270];
    int sel = s ? ff_selected_file(s) : -1;
    if (!s || sel < 0 || !(s->items[sel].entry.attr & ATTR_DIRECTORY)) {
        menu_paste();
        return;
    }
    item_fspath(s, s->items[sel].name, p, sizeof(p));
    const char *err = fo_paste(p);
    fo_report(s->win, "Paste", err);
    if (!err)
        strcpy(s->status, "Pasted.");
}

/* ── Trash ── */
static void ctx_put_back(void) {
    ff_inst_t *s = active_finder();
    char p[270];
    if (s && sel_path(s, p, sizeof(p))) {
        const char *err = fo_put_back(p);
        fo_report(s->win, "Put Back", err);
        if (!err)
            strcpy(s->status, "Put back.");
    }
}

static void ctx_empty_trash(void) {
    ff_inst_t *s = active_finder();
    fo_ask_empty_trash(s ? s->win : NULL, NULL, NULL);
}

static void view_context_menu(ff_inst_t *s, control *c) {
    int hit;
    if (ctl_menu_from_key()) {
        hit = ff_selected(s);
        if (hit >= 0) { /* show it by the item */
            c->mx = 2 + s->items[hit].icon_x + ICON_CELL_W / 2;
            c->my = 2 + s->items[hit].icon_y - s->scroll_y + ICON_H;
        }
    } else {
        hit = ff_hit(s, c->ax + c->mx, c->ay + c->my);
        ff_select(s, hit);
    }
    menu *m = popup_menu();
    ff_item_t *it = hit >= 0 ? &s->items[hit] : NULL;
    if (fo_is_trash(s->path) && !(it && it->is_dotdot)) { /* in the Trash */
        if (it) {
            menu_add_item(m, "Put Back", ctx_put_back);
            menu_add_item(m, "Delete Now...", menu_delete);
            menu_add_separator(m);
            menu_add_item(m, "Get Info", menu_get_info);
        } else {
            menu_add_item(m, "Empty Trash...", ctx_empty_trash);
            menu_enable_last(m, s->item_count > 1);
        }
        popup_show_for(c);
        return;
    }
    if (it && (it->is_dotdot || it->is_vdrive)) {
        menu_add_item(m, "Open", ctx_open);
        if (it->is_vdrive) {
            menu_add_item(m, "Get Info", menu_get_info);
            if (it->vdrive_path[1] == 'F') { /* /FDD0, /FDD1 */
                menu_add_separator(m);
                menu_add_item(m, "Eject Floppy", menu_eject_fdd);
            }
        }
    } else if (it) {
        bool dir = (it->entry.attr & ATTR_DIRECTORY) != 0;
        bool prot = path_is_protected(it->name);
        menu_add_item(m, "Open", ctx_open);
        menu_add_separator(m);
        menu_add_item(m, "Cut", menu_cut);
        menu_enable_last(m, !prot);
        menu_add_item(m, "Copy", menu_copy);
        menu_enable_last(m, !prot);
        if (dir) {
            menu_add_item(m, "Paste Into", ctx_paste_into);
            menu_enable_last(m, clipboard_has_file());
        }
        menu_add_separator(m);
        menu_add_item(m, "Rename", menu_rename);
        menu_enable_last(m, !prot);
        menu_add_item(m, "Delete", menu_delete);
        menu_enable_last(m, !prot);
        menu_add_item(m, it->hidden ? "Unhide" : "Hide", menu_toggle_hidden);
        menu_enable_last(m, !prot);
        menu_add_separator(m);
        menu_add_item(m, "Get Info", menu_get_info);
    } else if (s->at_vroot) {
        menu_add_item(m, "Insert Floppy", menu_insert_fdd);
        menu_add_item(m, "Reload", menu_reload);
    } else {
        menu_add_item(m, "New File", menu_new_file);
        menu_add_item(m, "New Folder", menu_new_folder);
        menu_add_separator(m);
        menu_add_item(m, "Paste", menu_paste);
        menu_enable_last(m, clipboard_has_file());
        menu_add_separator(m);
        menu_add_item(m, "Copy Path", menu_copy_path);
        menu_add_item(m, s->show_hidden ? "Hide Hidden" : "Show Hidden",
                      menu_show_hidden_toggle);
        menu_add_item(m, "Reload", menu_reload);
    }
    popup_show_for(c);
}

static void on_view(control *c, int ev) {
    ff_inst_t *s = c->user;
    int hit;
    switch (ev) {
    case EV_MOUSEDOWN:
        hit = ff_hit(s, mouse.x, mouse.y);
        ff_select(s, hit);
        s->press_idx = hit;
        s->press_x = mouse.x;
        s->press_y = mouse.y;
        break;
    case EV_DBLCLICK:
        hit = ff_hit(s, mouse.x, mouse.y);
        s->press_idx = -1;
        if (hit >= 0) {
            ff_select(s, hit);
            ff_open_item(s, hit);
        }
        break;
    case EV_MOUSEMOVE: {
        if (s->drag_idx >= 0 || s->press_idx < 0 || s->at_vroot)
            break;
        int dx = mouse.x - s->press_x, dy = mouse.y - s->press_y;
        ff_item_t *it = &s->items[s->press_idx];
        if (dx * dx + dy * dy < 9 || it->is_dotdot || it->is_vdrive ||
            path_is_protected(it->name))
            break;
        it->dragging = true;
        s->drag_idx = s->press_idx;
        wm_set_overlay(draw_drag_ghost, s);
        break;
    }
    case EV_MOUSEUP:
        s->press_idx = -1;
        if (s->drag_idx >= 0)
            view_drop(s);
        break;
    case EV_KEY:
        view_key(s);
        break;
    case EV_MENU:
        view_context_menu(s, c);
        break;
    }
    ff_sync(s);
}

static void on_sb(control *c, int ev) {
    if (ev == EV_CHANGE)
        ((ff_inst_t *)c->user)->scroll_y = ctl_value(c);
}

/* ── Toolbar ──────────────────────────────────────────────────────── */
static void on_nav(control *c, int ev) {
    ff_inst_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    if (c == s->back)
        ff_history_go(s, s->history_index - 1);
    else if (c == s->fwd)
        ff_history_go(s, s->history_index + 1);
    else if (c == s->up)
        ff_go_up(s);
    else if (c == s->home)
        ff_go_vroot(s);
    else
        ff_go_desktop(s);
    ff_sync(s);
}

static void on_path(control *c, int ev) {
    ff_inst_t *s = c->user;
    if (ev != EV_SUBMIT)
        return;
    char p[256];
    strncpy(p, ctl_text(c), sizeof(p) - 1);
    p[sizeof(p) - 1] = 0;
    int n = (int)strlen(p);
    while (n > 1 && p[n - 1] == '/')
        p[--n] = 0;
    uint8_t drv;
    uint16_t cl;
    if (!p[0] || is_vroot_path(p))
        ff_go_vroot(s);
    else if (resolve_dir(p, &drv, &cl))
        ff_navigate(s, cl, p, drv);
    else {
        ctl_set_text(c, s->path);
        dlg_message(s->win, "FileF", "Folder not found.");
        return;
    }
    ctl_focus(s->view);
}

/* ── Folder tree pane ─────────────────────────────────────────────── */

/* "/HDA/A/B" for a node: drive label at the root, then folder names. */
static void tree_fill_roots(ff_inst_t *s) {
    tree_clear(s->tree);
    s->tree_at[0] = 0; /* re-sync */
    for (int i = 0; i < VDRIVE_COUNT; i++)
        if (fs_drive_mounted(k_vdrives[i].drive_id))
            tree_add(s->tree, -1, k_vdrives[i].label, k_vdrives[i].drive_id,
                     true);
}

/* Called on every ff_sync (often): only does work when the folder shown
 * changed, as finding a path may read directories. */
static void tree_sync(ff_inst_t *s) {
    if (!s->tree || !s->tree_shown || !strcmp(s->tree_at, s->path))
        return;
    strncpy(s->tree_at, s->path, sizeof(s->tree_at) - 1);
    tree_set_sel(s->tree, s->at_vroot ? -1 : tree_find_path(s->tree, s->path));
}

/* A folder's contents changed: re-read that branch if it is open. */
static void tree_refresh_dir(ff_inst_t *s, const char *dir) {
    if (!s->tree || !s->tree_shown)
        return;
    char v[270];
    fo_vpath(dir, v, sizeof(v));
    int node = tree_find_path(s->tree, v);
    if (node >= 0 && tree_is_expanded(s->tree, node)) {
        int sel = tree_sel(s->tree);
        tree_clear_children(s->tree, node);
        tree_expand(s->tree, node, true);
        (void)sel;
        s->tree_at[0] = 0;
        tree_sync(s);
    }
}

static void ctx_folder_info(void) {
    ff_inst_t *s = active_finder();
    if (s && !s->at_vroot)
        fo_show_info(s->win, s->path);
}

static void tree_context_menu(ff_inst_t *s, control *c) {
    if (tree_sel(c) < 0 || s->at_vroot)
        return;
    menu *m = popup_menu();
    menu_add_item(m, "New File", menu_new_file);
    menu_add_item(m, "New Folder", menu_new_folder);
    menu_add_separator(m);
    menu_add_item(m, "Paste", menu_paste);
    menu_enable_last(m, clipboard_has_file());
    menu_add_item(m, "Copy Path", menu_copy_path);
    menu_add_separator(m);
    menu_add_item(m, "Get Info", ctx_folder_info);
    popup_show_for(c);
}

static void on_tree(control *c, int ev) {
    ff_inst_t *s = c->user;
    if (ev == EV_EXPAND) {
        int node = tree_event_node(c);
        char path[270];
        tree_path(c, node, path, sizeof(path));
        static char names[64][13];
        int n = fo_list_dirs(path, names, 64, s->show_hidden);
        for (int i = 0; i < n; i++)
            tree_add(c, node, names[i], 0, true);
    } else if (ev == EV_CHANGE) {
        int node = tree_sel(c);
        if (node < 0)
            return;
        char path[270];
        tree_path(c, node, path, sizeof(path));
        uint8_t drv;
        uint16_t cl;
        if (strcmp(path, s->path) && resolve_dir(path, &drv, &cl))
            ff_navigate(s, cl, path, drv);
    } else if (ev == EV_MENU) {
        tree_context_menu(s, c);
    }
}

static void set_tree_shown(ff_inst_t *s, bool on) {
    int wx, wy, cw, ch;
    win_client_rect(s->win, &wx, &wy, &cw, &ch);
    s->tree_shown = on;
    ctl_show(s->tree, on);
    int vx = on ? 2 + TREE_W + 2 : 2;
    /* current place and height (y/h from creation are relative to the
     * window's size back then) */
    ctl_move(s->view, vx, s->view->ay - wy, cw - 12 - vx + 2, s->view->ah);
    if (on) {
        tree_fill_roots(s);
        tree_sync(s);
    } else if (s->win->focus == s->tree) {
        ctl_focus(s->view);
    }
    s->last_view_w = -1; /* relayout icons */
}

static void menu_toggle_tree(void) {
    ff_inst_t *s = active_finder();
    if (s) {
        set_tree_shown(s, !s->tree_shown);
        app_prefs_set_int("FILEF", "tree", s->tree_shown);
    }
}

/* ── Init / frame / destroy ───────────────────────────────────────── */
static void ff_init(void *state) {
    ff_inst_t *s = state;
    int x = FF_DEFAULT_X + (s_cascade % 6) * FF_CASCADE;
    int y = FF_DEFAULT_Y + (s_cascade % 6) * FF_CASCADE;
    s_cascade++;
    s->win = app_window(s, "FileF", x, y, FF_DEFAULT_W, FF_DEFAULT_H, DARK_GRAY,
                        AW_RESIZABLE);
    if (!s->win)
        return;
    window *w = s->win;
    int wx, wy, cw, ch;
    win_client_rect(w, &wx, &wy, &cw, &ch);

    s->back = ctl_button(w, 2, 2, 14, 12, "<", 0, on_nav);
    s->fwd = ctl_button(w, 17, 2, 14, 12, ">", 0, on_nav);
    s->up = ctl_button(w, 32, 2, 14, 12, "^", 0, on_nav);
    s->home = ctl_button(w, 47, 2, 24, 12, "Home", 0, on_nav);
    s->desk = ctl_button(w, 72, 2, 24, 12, "Desk", 0, on_nav);
    ctl_set_tooltip(s->back, "Back");
    ctl_set_tooltip(s->fwd, "Forward");
    ctl_set_tooltip(s->up, "Up one folder");
    ctl_set_tooltip(s->home, "All drives");
    ctl_set_tooltip(s->desk, "Desktop folder");
    control *tb[] = {s->back, s->fwd, s->up, s->home, s->desk};
    for (int i = 0; i < 5; i++)
        ctl_style_add(tb[i], CS_NOFOCUS);
    s->path_edit = ctl_edit(w, 98, 2, cw - 100, 12, "/", 255,
                            ES_UPPER | CS_STRETCH_W, on_path);
    s->tree =
        ctl_tree(w, 2, 16, TREE_W, ch - 29, TS_FOLDERS | CS_STRETCH_H, on_tree);
    s->view =
        ctl_custom(w, 2, 16, cw - 12, ch - 29, paint_view, on_view,
                   CS_BORDER | CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);
    s->sb = ctl_vscroll(w, cw - 10, 16, ch - 29, on_sb);
    ctl_style_add(s->sb, CS_ANCHOR_RIGHT | CS_STRETCH_H | CS_NOFOCUS);
    s->status_lbl = ctl_status(w, "");

    menu *m = window_add_menu(w, "File");
    menu_add_item(m, "New File", menu_new_file);
    menu_add_item(m, "New Folder", menu_new_folder);
    menu_add_item(m, "Rename", menu_rename);
    menu_add_item(m, "Get Info", menu_get_info);
    menu_add_item(m, "Reload", menu_reload);
    menu_add_separator(m);
    menu_add_item(m, "About FileF", menu_about_filef);
    menu_add_separator(m);
    menu_add_item(m, "Close", menu_close);
    m = window_add_menu(w, "Edit");
    menu_add_item(m, "Copy", menu_copy);
    menu_add_item(m, "Cut", menu_cut);
    menu_add_item(m, "Paste", menu_paste);
    menu_add_separator(m);
    menu_add_item(m, "Delete", menu_delete);
    menu_add_item(m, "Copy Path", menu_copy_path);
    menu_add_separator(m);
    menu_add_item(m, "Toggle Hidden", menu_toggle_hidden);
    m = window_add_menu(w, "View");
    menu_add_item(m, "By Name", menu_sort_name);
    menu_add_item(m, "By Date", menu_sort_date);
    menu_add_item(m, "By Size", menu_sort_size);
    menu_add_separator(m);
    menu_add_item(m, "Show Hidden", menu_show_hidden_toggle);
    menu_add_item(m, "Folder Tree", menu_toggle_tree);
    m = window_add_menu(w, "Drive");
    menu_add_item(m, "Insert Floppy", menu_insert_fdd);
    menu_add_item(m, "Eject Floppy", menu_eject_fdd);

    s->drive_id = DRIVE_HDA;
    s->sort = SORT_NAME;
    s->drag_idx = -1;
    s->press_idx = -1;
    s->at_vroot = true;
    strcpy(s->path, "/");
    w->title = s->path;
    strcpy(s->history[0], "/");
    s->history_index = 0;
    s->history_count = 1;
    /* saved view options (os/prefs.h) */
    s->show_hidden = app_prefs_get_int("FILEF", "hidden", 0) != 0;
    set_tree_shown(s, app_prefs_get_int("FILEF", "tree", 1) != 0);
    ff_reload(s);
    ctl_focus(s->view);
    s_last_active = s;
}

static void ff_on_frame(void *state) {
    ff_inst_t *s = state;
    if (!s->win)
        return;
    if (wm_focused_window() == s->win)
        s_last_active = s;
    if (!s->at_vroot && desktop_fs_is_dirty()) {
        char dp[270];
        build_vpath(DRIVE_HDA, "/DESKTOP", dp, sizeof(dp));
        if (strcmp(s->path, dp) == 0)
            ff_reload(s);
    }
    ff_sync(s);
}

static void ff_destroy(void *state) {
    wm_clear_overlay(state);
    if (s_last_active == state)
        s_last_active = NULL;
}

app_descriptor filef_app = {
    .name = "FILEF",
    .state_size = sizeof(ff_inst_t),
    .init = ff_init,
    .on_frame = ff_on_frame,
    .destroy = ff_destroy,
};
