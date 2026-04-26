#include "config/runtime_config.h"
#include "os/api.h"
#include "os/clipboard.h"
#include "ui/desktop.h"
#include "ui/desktop_fs.h"

#define FF_DEFAULT_W 200
#define FF_DEFAULT_H 150
#define FF_DEFAULT_X 40
#define FF_DEFAULT_Y 20
#define FF_CASCADE 16

#define ICON_CELL_W 44
#define ICON_CELL_H 30
#define CONTENT_TOP 6
#define CONTENT_BOT 4
#define STATUS_H 8
#define CHAR_W 5
#define CHAR_H 6
#define LABEL_CHARS 8
#define SCROLLBAR_W 6

#define MAX_ENTRIES 128
#define SORT_NAME 0
#define SORT_DATE 1
#define SORT_SIZE 2
#define MODE_NORMAL 0
#define MODE_RENAME 1
#define MODE_CONFIRM 2
#define MODE_NEWNAME 3
#define MODE_INFO 4

/* ── Virtual root drive entries ─────────────────────────────────────────── */
typedef struct {
    const char *vpath; /* e.g. "/HDA" */
    const char *label; /* display label */
    uint8_t drive_id;
    bool is_hdd; /* true=HDD icon, false=floppy icon */
} vdrive_t;

static const vdrive_t k_vdrives[] = {
    {"/HDA", "HDA", DRIVE_HDA, true},
    {"/HDB", "HDB", DRIVE_HDB, true},
    {"/FDD0", "FDD0", DRIVE_FDD0, false},
    {"/FDD1", "FDD1", DRIVE_FDD1, false},
};
#define VDRIVE_COUNT ((int)(sizeof(k_vdrives) / sizeof(k_vdrives[0])))

/* ── Item ───────────────────────────────────────────────────────────────── */
typedef struct {
    dir_entry_t entry;
    char name[13];
    char label[LABEL_CHARS + 1];
    bool is_dotdot;
    bool is_vdrive; /* virtual root drive entry */
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

/* ── Instance ───────────────────────────────────────────────────────────── */
typedef struct {
    window *win;
    uint16_t dir_cluster;
    char path[256]; /* current logical path */
    uint8_t drive_id;
    bool at_vroot; /* showing the virtual "/" with drive icons */

    ff_item_t items[MAX_ENTRIES];
    int item_count;

    int sort;
    int mode;

    char rename_buf[13];
    int rename_len;
    int rename_idx;

    char newname_buf[13];
    int newname_len;
    bool newname_is_dir;
    bool newname_hidden;

    char confirm_msg[80];
    void (*confirm_action)(void *inst_state);

    int drag_idx;
    uint32_t last_click_tick;
    int last_click_idx;

    char status[48];

    char info_name[13];
    char info_path[270];
    char info_type[32];
    char info_size[32];
    char info_date[32];
    char info_time[32];

    int scroll_y;
    int content_h_total;
    int last_win_w;
    int last_win_h;

    bool show_hidden;
} ff_inst_t;

app_descriptor filef_app;

static int s_cascade = 0;
static ff_inst_t *s_last_active = NULL;

/* ── Forward declarations ───────────────────────────────────────────────── */
static void ff_reload(ff_inst_t *s);
static void ff_draw_window(window *win, void *ud);
static void ff_navigate(ff_inst_t *s, uint16_t cluster, const char *path,
                        uint8_t drive_id);
static void ff_go_vroot(ff_inst_t *s);

/* ── Path/drive helpers ─────────────────────────────────────────────────── */

/* Is this path the virtual root? */
static bool is_vroot_path(const char *path) {
    return path[0] == '/' && path[1] == '\0';
}

/* Map a /HDA/... path to drive + sub-path on that drive.
   Returns true if path starts with a known /DRV prefix.
   out_sub is the remainder after /DRV (starts with '/'), or "/" if at root. */
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

/* Build full logical path: /DRV + sub (sub starts with '/') */
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

/* Resolve logical path to (drive_id, cluster) for a DIRECTORY.
   path must start with /DRV/... or be /DRV.
   Returns false if drive not mounted or path not found. */
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
    /* Walk sub path on drive */
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

/* ── Instance helpers ───────────────────────────────────────────────────── */
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

/* ── notify desktop if path matches ────────────────────────────────────── */
static void maybe_notify_desktop(ff_inst_t *s) {
    /* desktop lives at /HDA/DESKTOP */
    char dp[270];
    build_vpath(DRIVE_HDA, "/DESKTOP", dp, sizeof(dp));
    if (strcmp(s->path, dp) == 0)
        desktop_fs_set_dirty();
}

/* ── Name validation ────────────────────────────────────────────────────── */
static bool fat_char_ok(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9'))
        return true;
    const char *ok = "!#$%&'()-@^_`{}~";
    for (int i = 0; ok[i]; i++)
        if (c == ok[i])
            return true;
    return false;
}

static bool validate_fat_name(const char *n, bool is_dir, const char **err) {
    if (!n || !n[0]) {
        *err = "Name cannot be empty.";
        return false;
    }
    const char *dot = strchr(n, '.');
    int base_len = dot ? (int)(dot - n) : (int)strlen(n);
    int ext_len = dot ? (int)strlen(dot + 1) : 0;
    if (is_dir && ext_len > 0) {
        *err = "Folder: no extension.";
        return false;
    }
    if (ext_len > 3) {
        *err = "Extension too long (max 3).";
        return false;
    }
    if (base_len == 0) {
        *err = "Base name empty.";
        return false;
    }
    if (base_len > 8) {
        *err = "Base name too long (8).";
        return false;
    }
    for (int i = 0; i < base_len; i++)
        if (!fat_char_ok(n[i])) {
            *err = "Invalid character.";
            return false;
        }
    for (int i = 0; i < ext_len; i++)
        if (!fat_char_ok(dot[1 + i])) {
            *err = "Invalid ext char.";
            return false;
        }
    return true;
}

/* ── Sort ───────────────────────────────────────────────────────────────── */
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

/* ── Layout ─────────────────────────────────────────────────────────────── */
static int content_view_h(ff_inst_t *s) {
    return s->win->h - 16 - CONTENT_TOP - CONTENT_BOT - STATUS_H - 1;
}

static void ff_layout(ff_inst_t *s) {
    int ww = s->win->w - 2 - SCROLLBAR_W - 2;
    int cols = (ww - 4) / ICON_CELL_W;
    if (cols < 1)
        cols = 1;
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

/* ── Reload ─────────────────────────────────────────────────────────────── */
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

    /* Virtual root: show mounted drives */
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
        return;
    }

    /* Normal directory */
    /* Show ".." unless we're at the root of a drive (/HDA, /HDB, etc.) */
    {
        uint8_t dummy_drive;
        char sub[256];
        bool has_vpath = resolve_vpath(s->path, &dummy_drive, sub, sizeof(sub));
        bool at_drive_root = has_vpath && strcmp(sub, "/") == 0;
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
    fs_list_dir(s->drive_id, s->dir_cluster, raw, MAX_ENTRIES, &count);

    for (int i = 0; i < count && s->item_count < MAX_ENTRIES; i++) {
        if (raw[i].name[0] == '.')
            continue;
        bool is_hidden = (raw[i].attr & ATTR_HIDDEN) != 0;
        if (is_hidden && !s->show_hidden)
            continue;
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
}

/* ── Navigation ─────────────────────────────────────────────────────────── */
static void ff_go_vroot(ff_inst_t *s) {
    s->at_vroot = true;
    s->dir_cluster = 0;
    s->drive_id = DRIVE_HDA;
    strcpy(s->path, "/");
    s->win->title = s->path;
    s->scroll_y = 0;
    ff_reload(s);
}

static void ff_navigate(ff_inst_t *s, uint16_t cluster, const char *path,
                        uint8_t drive_id) {
    s->at_vroot = false;
    s->dir_cluster = cluster;
    s->drive_id = drive_id;
    strncpy(s->path, path, 255);
    s->path[255] = '\0';
    s->win->title = s->path;
    s->scroll_y = 0;
    ff_reload(s);
}

/* Public open: called from desktop and other apps.
   path is a logical /DRV/sub path or "/" for vroot. */
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

/* ── Drawing ─────────────────────────────────────────────────────────────── */
static void ff_draw_item(ff_inst_t *s, int i, int base_x, int base_y,
                         int clip_bottom) {
    ff_item_t *it = &s->items[i];
    int ax = base_x + it->icon_x + (ICON_CELL_W - ICON_SZ_W) / 2;
    int ay = base_y + it->icon_y;
    int content_top_y = s->win->y + MENUBAR_H + 16 + CONTENT_TOP;
    if (ay < content_top_y)
        return;
    if (ay + ICON_SZ_H + CHAR_H + 2 > clip_bottom)
        return;

    if (it->is_vdrive) {
        if (it->vdrive_hdd)
            draw_hdd_icon(ax, ay, it->selected);
        else
            draw_floppy_icon(ax, ay, it->selected);
    } else if (it->is_dotdot) {
        draw_dotdot_icon(ax, ay, it->selected);
    } else if (it->entry.attr & ATTR_DIRECTORY) {
        draw_folder_icon(ax, ay, it->selected);
    } else if (item_is_app(it)) {
        draw_app_icon(ax, ay, it->selected);
    } else {
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
        draw_char(ax + ICON_SZ_W - 6, ay, 'H', DARK_GRAY, 2);

    bool is_cut =
        (!it->is_vdrive && !it->is_dotdot && clipboard_has_file() &&
         g_clipboard.is_cut && strcmp(g_clipboard.name, it->name) == 0 &&
         strcmp(g_clipboard.src_path, s->path) == 0);

    int lw = (int)strlen(it->label) * CHAR_W;
    int lx = ax + ICON_SZ_W / 2 - lw / 2;
    int ly = ay + ICON_SZ_H + 1;
    uint8_t lc = (is_cut || it->hidden) ? DARK_GRAY : BLACK;
    if (it->selected) {
        fill_rect(lx - 1, ly - 1, lw + 2, CHAR_H + 2, BLACK);
        draw_string(lx, ly, it->label, WHITE, 2);
    } else {
        draw_string(lx, ly, it->label, lc, 2);
    }

    if (s->mode == MODE_RENAME && i == s->rename_idx) {
        fill_rect(ax - 1, ay + ICON_SZ_H, ICON_CELL_W, CHAR_H + 2, WHITE);
        draw_rect(ax - 1, ay + ICON_SZ_H, ICON_CELL_W, CHAR_H + 2, BLACK);
        draw_string(ax, ay + ICON_SZ_H + 1, s->rename_buf, BLACK, 2);
        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 50) % 2 == 0) {
            int cx = ax + s->rename_len * CHAR_W;
            draw_string(cx, ay + ICON_SZ_H + 1, "|", BLACK, 2);
        }
    }
}

static void ff_draw_info_overlay(ff_inst_t *s, int wx, int wy, int ww, int wh) {
    int pw = ww - 16, ph = wh - 20, px = wx + 8, py = wy + 10;
    fill_rect(px + 3, py + 3, pw, ph, BLACK);
    fill_rect(px, py, pw, ph, LIGHT_GRAY);
    draw_rect(px, py, pw, ph, BLACK);
    fill_rect(px, py, pw, 12, DARK_GRAY);
    draw_string(px + 4, py + 3, "Get Info", WHITE, 2);
    fill_rect(px + pw - 14, py + 2, 10, 8, RED);
    draw_rect(px + pw - 14, py + 2, 10, 8, BLACK);
    draw_string(px + pw - 12, py + 3, "X", WHITE, 2);
    draw_line(px, py + 13, px + pw - 1, py + 13, BLACK);
    int row_y = py + 17, lx2 = px + 4, vx = px + 50, line_h = 10;
    draw_string(lx2, row_y, "Name:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_name, BLACK, 2);
    row_y += line_h;
    draw_string(lx2, row_y, "Path:", DARK_GRAY, 2);
    {
        int mc = (pw - 54) / CHAR_W, plen = (int)strlen(s->info_path);
        if (plen <= mc) {
            draw_string(vx, row_y, s->info_path, BLACK, 2);
        } else {
            char tr[64];
            strcpy(tr, "...");
            int start = plen - (mc - 3);
            if (start < 0)
                start = 0;
            strncat(tr, s->info_path + start, 60);
            draw_string(vx, row_y, tr, BLACK, 2);
        }
    }
    row_y += line_h;
    draw_string(lx2, row_y, "Type:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_type, BLACK, 2);
    row_y += line_h;
    draw_string(lx2, row_y, "Size:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_size, BLACK, 2);
    row_y += line_h;
    draw_string(lx2, row_y, "Date:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_date, BLACK, 2);
    row_y += line_h;
    draw_string(lx2, row_y, "Time:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_time, BLACK, 2);
    row_y += line_h;
    draw_string(lx2, row_y, "Hidden:", DARK_GRAY, 2);
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected) {
            draw_string(vx, row_y, s->items[i].hidden ? "Yes" : "No", BLACK, 2);
            break;
        }
    int btn_x = px + pw / 2 - 20, btn_y = py + ph - 14;
    fill_rect(btn_x, btn_y, 40, 10, LIGHT_BLUE);
    draw_rect(btn_x, btn_y, 40, 10, BLACK);
    draw_string(btn_x + 13, btn_y + 2, "OK", WHITE, 2);
}

static void ff_populate_info(ff_inst_t *s, int sel) {
    ff_item_t *it = &s->items[sel];
    strncpy(s->info_name, it->name, 12);
    s->info_name[12] = '\0';
    snprintf(s->info_path, sizeof(s->info_path), "%s/%s", s->path, it->name);
    if (it->is_vdrive) {
        strcpy(s->info_type, "Drive");
        strcpy(s->info_size, "-");
    } else if (it->is_dotdot) {
        strcpy(s->info_type, "Parent Dir");
        strcpy(s->info_size, "-");
    } else if (it->entry.attr & ATTR_DIRECTORY) {
        strcpy(s->info_type, "Folder");
        strcpy(s->info_size, "-");
    } else if (item_is_app(it)) {
        strcpy(s->info_type, "Application");
        sprintf(s->info_size, "%u B", it->entry.file_size);
    } else {
        const char *dot = strchr(it->name, '.');
        const char *ext = dot ? dot + 1 : "";
        if (!strcmp(ext, "TXT") || !strcmp(ext, "txt"))
            strcpy(s->info_type, "Text File");
        else if (!strlen(ext))
            strcpy(s->info_type, "File");
        else {
            strcpy(s->info_type, ext);
            strcat(s->info_type, " File");
        }
        sprintf(s->info_size, "%u B", it->entry.file_size);
    }
    uint16_t fd = it->entry.modify_date, ft = it->entry.modify_time;
    if (!fd) {
        strcpy(s->info_date, "Unknown");
        strcpy(s->info_time, "Unknown");
    } else {
        uint16_t yr = ((fd >> 9) & 0x7F) + 1980;
        uint8_t mo = (fd >> 5) & 0x0F, dy = fd & 0x1F, hr = (ft >> 11) & 0x1F,
                mn = (ft >> 5) & 0x3F, sc = (ft & 0x1F) * 2;
        sprintf(s->info_date, "%04d/%02d/%02d", yr, mo, dy);
        sprintf(s->info_time, "%02d:%02d:%02d", hr, mn, sc);
    }
}

static void ff_draw_scrollbar(ff_inst_t *s, int wx, int wy, int ww, int wh) {
    int view_h = wh - CONTENT_TOP - CONTENT_BOT - STATUS_H + 1;
    if (view_h < 1)
        view_h = 1;
    int sb_x = wx + ww - SCROLLBAR_W - 1, sb_y = wy + 2, sb_h = view_h;
    fill_rect(sb_x, sb_y, SCROLLBAR_W, sb_h, LIGHT_GRAY);
    if (s->content_h_total > view_h) {
        int ms = s->content_h_total - view_h;
        if (ms < 1)
            ms = 1;
        int th = (view_h * sb_h) / s->content_h_total;
        if (th < 6)
            th = 6;
        if (th > sb_h)
            th = sb_h;
        int tr = sb_h - th, ty = sb_y;
        if (tr > 0)
            ty += (s->scroll_y * tr) / ms;
        fill_rect(sb_x + 1, ty + 1, SCROLLBAR_W - 2, th - 2, DARK_GRAY);
    }
}

static void ff_draw_window(window *win, void *ud) {
    ff_inst_t *s = (ff_inst_t *)ud;
    if (!s)
        return;
    int wx = win->x + 1, wy = win->y + MENUBAR_H + 16, ww = win->w - 2,
        wh = win->h - 16;
    int content_h = wh - CONTENT_TOP - CONTENT_BOT - STATUS_H;
    int stat_y = wy + wh - STATUS_H - CONTENT_BOT;

    fill_rect(wx, stat_y, ww, STATUS_H + CONTENT_BOT, LIGHT_GRAY);
    draw_line(wx, stat_y, wx + ww - 1, stat_y, LIGHT_GRAY);

    char stat[64];
    if (s->status[0]) {
        strncpy(stat, s->status, 63);
        stat[63] = '\0';
    } else {
        int real = s->item_count;
        if (!s->at_vroot) {
            /* subtract dotdot if present */
            for (int i = 0; i < s->item_count; i++)
                if (s->items[i].is_dotdot) {
                    real--;
                    break;
                }
        }
        sprintf(stat, "%d item%s%s", real, real == 1 ? "" : "s",
                s->show_hidden ? " (hidden shown)" : "");
    }
    draw_string(wx + 3, stat_y + 1, stat, DARK_GRAY, 2);

    int clip_bottom = stat_y;
    for (int i = 0; i < s->item_count; i++) {
        if (s->items[i].dragging)
            continue;
        ff_draw_item(s, i, wx, wy - s->scroll_y, clip_bottom);
    }
    if (s->drag_idx >= 0) {
        ff_item_t *it = &s->items[s->drag_idx];
        int ax = mouse.x - it->drag_off_x, ay = mouse.y - it->drag_off_y;
        int svx = it->icon_x, svy = it->icon_y;
        it->icon_x = ax - wx;
        it->icon_y = ay - wy + s->scroll_y;
        ff_draw_item(s, s->drag_idx, wx, wy - s->scroll_y, clip_bottom);
        it->icon_x = svx;
        it->icon_y = svy;
    }

    ff_draw_scrollbar(s, wx, wy, ww, wh);

    if (s->mode == MODE_CONFIRM) {
        int bx = wx + 4, by = wy + content_h / 2 - 16, bw = ww - 8;
        fill_rect(bx, by, bw, 34, LIGHT_GRAY);
        draw_rect(bx, by, bw, 34, BLACK);
        draw_string(bx + 4, by + 4, s->confirm_msg, BLACK, 2);
        fill_rect(bx + 4, by + 20, 28, 10, BLACK);
        draw_rect(bx + 4, by + 20, 28, 10, BLACK);
        draw_string(bx + 8, by + 22, "Yes", WHITE, 2);
        fill_rect(bx + 36, by + 20, 28, 10, LIGHT_GRAY);
        draw_rect(bx + 36, by + 20, 28, 10, BLACK);
        draw_string(bx + 40, by + 22, "No", BLACK, 2);
    }
    if (s->mode == MODE_NEWNAME) {
        int bx = wx + 4, by = wy + content_h / 2 - 16, bw = ww - 8;
        fill_rect(bx, by, bw, 44, LIGHT_GRAY);
        draw_rect(bx, by, bw, 44, BLACK);
        draw_string(bx + 4, by + 4,
                    s->newname_is_dir ? "Folder name:" : "File name:", BLACK,
                    2);
        fill_rect(bx + 4, by + 14, bw - 8, 10, WHITE);
        draw_rect(bx + 4, by + 14, bw - 8, 10, BLACK);
        draw_string(bx + 6, by + 15, s->newname_buf, BLACK, 2);
        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 50) % 2 == 0) {
            int cx = bx + 6 + s->newname_len * CHAR_W,
                mcx = bx + bw - 8 - CHAR_W - 2;
            if (cx < mcx)
                draw_string(cx, by + 15, "|", BLACK, 2);
        }
        fill_rect(bx + 4, by + 28, 10, 10, WHITE);
        draw_rect(bx + 4, by + 28, 10, 10, BLACK);
        if (s->newname_hidden)
            draw_string(bx + 6, by + 29, "x", BLACK, 2);
        draw_string(bx + 17, by + 29, "Hidden", BLACK, 2);
    }
    if (s->mode == MODE_INFO)
        ff_draw_info_overlay(s, wx, wy, ww, wh);
}

/* ── Hit testing ─────────────────────────────────────────────────────────── */
static bool in_content(ff_inst_t *s, int mx, int my) {
    int wx = s->win->x + 1, wy = s->win->y + MENUBAR_H + 16 + CONTENT_TOP;
    int ww = s->win->w - 2 - SCROLLBAR_W - 2, wh = content_view_h(s);
    return mx >= wx && mx < wx + ww && my >= wy && my < wy + wh;
}

static int ff_hit(ff_inst_t *s, int mx, int my) {
    if (!in_content(s, mx, my))
        return -1;
    int wx = s->win->x + 1, wy = s->win->y + MENUBAR_H + 16 + CONTENT_TOP;
    for (int i = 0; i < s->item_count; i++) {
        ff_item_t *it = &s->items[i];
        int ax = wx + it->icon_x + (ICON_CELL_W - ICON_SZ_W) / 2;
        int ay = wy + it->icon_y - s->scroll_y;
        if (mx >= ax && mx < ax + ICON_SZ_W && my >= ay &&
            my < ay + ICON_SZ_H + CHAR_H + 4)
            return i;
    }
    return -1;
}

/* ── Helpers for operations ─────────────────────────────────────────────── */

/* Set dir_context to match current instance, save old */
#define FF_PUSH_CTX(s)                                                         \
    uint16_t _saved_cluster = dir_context.current_cluster;                     \
    uint8_t _saved_drive = dir_context.drive_id;                               \
    dir_context.current_cluster = (s)->dir_cluster;                            \
    dir_context.drive_id = (s)->drive_id;

#define FF_POP_CTX()                                                           \
    dir_context.current_cluster = _saved_cluster;                              \
    dir_context.drive_id = _saved_drive;

/* Build absolute fs path for an item name in the current dir.
   For vroot we don't call this. */
static void item_fspath(ff_inst_t *s, const char *name, char *out, int sz) {
    uint8_t drv;
    char sub[256];
    if (!resolve_vpath(s->path, &drv, sub, sizeof(sub))) {
        snprintf(out, sz, "%s/%s", strcmp(s->path, "/") == 0 ? "" : s->path,
                 name);
        return;
    }
    if (strcmp(sub, "/") == 0)
        snprintf(out, sz, "/%s", name);
    else
        snprintf(out, sz, "%s/%s", sub, name);
}

/* ── Delete / paste ─────────────────────────────────────────────────────── */
static void do_delete(void *ud) {
    ff_inst_t *s = (ff_inst_t *)ud;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot &&
            !s->items[i].is_vdrive) {
            sel = i;
            break;
        }
    if (sel < 0) {
        s->mode = MODE_NORMAL;
        return;
    }
    if (path_is_protected(s->items[sel].name)) {
        strcpy(s->status, "Protected item.");
        s->mode = MODE_NORMAL;
        return;
    }
    char fspath[270];
    item_fspath(s, s->items[sel].name, fspath, sizeof(fspath));
    FF_PUSH_CTX(s);
    bool ok = (s->items[sel].entry.attr & ATTR_DIRECTORY) ? fs_rm_rf(fspath)
                                                          : fs_delete(fspath);
    FF_POP_CTX();
    strcpy(s->status, ok ? "Deleted." : "Delete failed.");
    s->mode = MODE_NORMAL;
    maybe_notify_desktop(s);
    ff_reload(s);
}

static void cancel_confirm(void *ud) {
    ff_inst_t *s = (ff_inst_t *)ud;
    s->mode = MODE_NORMAL;
}

/* ── Menu handlers ──────────────────────────────────────────────────────── */
static void menu_new_file(void) {
    ff_inst_t *s = active_finder();
    if (!s || s->at_vroot)
        return;
    s->newname_buf[0] = '\0';
    s->newname_len = 0;
    s->newname_is_dir = false;
    s->newname_hidden = false;
    s->mode = MODE_NEWNAME;
}
static void menu_new_folder(void) {
    ff_inst_t *s = active_finder();
    if (!s || s->at_vroot)
        return;
    s->newname_buf[0] = '\0';
    s->newname_len = 0;
    s->newname_is_dir = true;
    s->newname_hidden = false;
    s->mode = MODE_NEWNAME;
}
static void menu_rename(void) {
    ff_inst_t *s = active_finder();
    if (!s || s->at_vroot)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot &&
            !s->items[i].is_vdrive) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
        return;
    }
    if (path_is_protected(s->items[sel].name)) {
        strcpy(s->status, "Protected.");
        return;
    }
    s->rename_idx = sel;
    strncpy(s->rename_buf, s->items[sel].name, 12);
    s->rename_buf[12] = '\0';
    s->rename_len = (int)strlen(s->rename_buf);
    s->mode = MODE_RENAME;
}
static void menu_get_info(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
        return;
    }
    ff_populate_info(s, sel);
    s->mode = MODE_INFO;
}
static void menu_reload(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    ff_reload(s);
}
static void menu_toggle_hidden(void) {
    ff_inst_t *s = active_finder();
    if (!s || s->at_vroot)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot &&
            !s->items[i].is_vdrive) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
        return;
    }
    if (path_is_protected(s->items[sel].name)) {
        strcpy(s->status, "Protected.");
        return;
    }
    char fspath[270];
    item_fspath(s, s->items[sel].name, fspath, sizeof(fspath));
    FF_PUSH_CTX(s);
    bool new_hidden = !s->items[sel].hidden;
    bool ok = fs_set_hidden(fspath, new_hidden);
    FF_POP_CTX();
    strcpy(s->status,
           ok ? (new_hidden ? "Marked hidden." : "Unmarked.") : "Failed.");
    ff_reload(s);
}
static void menu_show_hidden_toggle(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    s->show_hidden = !s->show_hidden;
    ff_reload(s);
}

/* ── Copy / cut / paste ─────────────────────────────────────────────────── */
static void menu_copy(void) {
    ff_inst_t *s = active_finder();
    if (!s || s->at_vroot)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot &&
            !s->items[i].is_vdrive) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
        return;
    }
    if (path_is_protected(s->items[sel].name)) {
        strcpy(s->status, "Protected.");
        return;
    }
    clipboard_set_file(s->path, s->items[sel].name,
                       (s->items[sel].entry.attr & ATTR_DIRECTORY) != 0, false,
                       s->drive_id);
    strcpy(s->status, "Copied.");
}
static void menu_cut(void) {
    ff_inst_t *s = active_finder();
    if (!s || s->at_vroot)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot &&
            !s->items[i].is_vdrive) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
        return;
    }
    if (path_is_protected(s->items[sel].name)) {
        strcpy(s->status, "Protected.");
        return;
    }
    clipboard_set_file(s->path, s->items[sel].name,
                       (s->items[sel].entry.attr & ATTR_DIRECTORY) != 0, true,
                       s->drive_id);
    strcpy(s->status, "Cut.");
}

static void menu_paste(void) {
    ff_inst_t *s = active_finder();
    if (!s || s->at_vroot)
        return;
    if (!clipboard_has_file()) {
        strcpy(s->status, "Clipboard empty.");
        modal_show(MODAL_INFO, "Paste", "Clipboard is empty.", NULL, NULL);
        return;
    }

    /* Build source absolute fs-path (drive-relative) */
    uint8_t src_drv;
    char src_sub[256];
    char src_fspath[270];
    if (!resolve_vpath(g_clipboard.src_path, &src_drv, src_sub,
                       sizeof(src_sub))) {
        snprintf(src_fspath, sizeof(src_fspath), "%s/%s",
                 strcmp(g_clipboard.src_path, "/") == 0 ? ""
                                                        : g_clipboard.src_path,
                 g_clipboard.name);
    } else {
        if (strcmp(src_sub, "/") == 0)
            snprintf(src_fspath, sizeof(src_fspath), "/%s", g_clipboard.name);
        else
            snprintf(src_fspath, sizeof(src_fspath), "%s/%s", src_sub,
                     g_clipboard.name);
    }

    /* Build dest absolute fs-path (drive-relative) */
    uint8_t dst_drv;
    char dst_sub[256];
    char dst_fspath[270];
    if (!resolve_vpath(s->path, &dst_drv, dst_sub, sizeof(dst_sub))) {
        snprintf(dst_fspath, sizeof(dst_fspath), "%s/%s",
                 strcmp(s->path, "/") == 0 ? "" : s->path, g_clipboard.name);
    } else {
        if (strcmp(dst_sub, "/") == 0)
            snprintf(dst_fspath, sizeof(dst_fspath), "/%s", g_clipboard.name);
        else
            snprintf(dst_fspath, sizeof(dst_fspath), "%s/%s", dst_sub,
                     g_clipboard.name);
    }

    if (src_drv == dst_drv && strcmp(g_clipboard.src_path, s->path) == 0 &&
        !g_clipboard.is_cut) {
        strcpy(s->status, "Already here.");
        modal_show(MODAL_INFO, "Paste", "File already in this folder.", NULL,
                   NULL);
        return;
    }

    /* Check name collision at dest */
    {
        uint16_t saved_c = dir_context.current_cluster;
        uint8_t saved_d = dir_context.drive_id;
        dir_context.current_cluster = s->dir_cluster;
        dir_context.drive_id = s->drive_id;
        dir_entry_t de;
        bool exists = fs_find(dst_fspath, &de);
        dir_context.current_cluster = saved_c;
        dir_context.drive_id = saved_d;
        if (exists) {
            strcpy(s->status, "Name already exists.");
            modal_show(MODAL_ERROR, "Paste Failed",
                       "A file with that name already exists here.", NULL,
                       NULL);
            return;
        }
    }

    bool ok;
    if (g_clipboard.is_cut) {
        /* For same-drive we can use fs_move; cross-drive falls back to
         * copy+delete */
        if (src_drv == dst_drv) {
            FF_PUSH_CTX(s);
            ok = g_clipboard.is_dir ? fs_move_dir(src_fspath, dst_fspath)
                                    : fs_move_file(src_fspath, dst_fspath);
            FF_POP_CTX();
        } else {
            /* cross-drive move = copy then delete */
            uint16_t saved_c = dir_context.current_cluster;
            uint8_t saved_d = dir_context.drive_id;
            dir_context.current_cluster = s->dir_cluster;
            dir_context.drive_id = dst_drv;
            ok = g_clipboard.is_dir ? fs_copy_dir(src_fspath, dst_fspath)
                                    : fs_copy_file(src_fspath, dst_fspath);
            dir_context.current_cluster = saved_c;
            dir_context.drive_id = saved_d;
            if (ok) {
                /* delete source */
                uint16_t sc2 = dir_context.current_cluster;
                uint8_t sd2 = dir_context.drive_id;
                dir_context.drive_id = src_drv;
                dir_context.current_cluster = 0;
                if (g_clipboard.is_dir)
                    fs_rm_rf(src_fspath);
                else
                    fs_delete(src_fspath);
                dir_context.current_cluster = sc2;
                dir_context.drive_id = sd2;
            }
        }
        if (ok)
            clipboard_clear();
    } else {
        uint16_t saved_c = dir_context.current_cluster;
        uint8_t saved_d = dir_context.drive_id;
        dir_context.current_cluster = s->dir_cluster;
        dir_context.drive_id = dst_drv;
        ok = g_clipboard.is_dir ? fs_copy_dir(src_fspath, dst_fspath)
                                : fs_copy_file(src_fspath, dst_fspath);
        dir_context.current_cluster = saved_c;
        dir_context.drive_id = saved_d;
    }

    if (ok) {
        strcpy(s->status, "Pasted.");
        maybe_notify_desktop(s);
        ff_reload(s);
        desktop_fs_set_dirty();
    } else {
        strcpy(s->status, "Paste failed.");
        modal_show(MODAL_ERROR, "Paste Failed", "Could not paste item.", NULL,
                   NULL);
    }
}

static void menu_delete(void) {
    ff_inst_t *s = active_finder();
    if (!s || s->at_vroot)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot &&
            !s->items[i].is_vdrive) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
        return;
    }
    if (path_is_protected(s->items[sel].name)) {
        strcpy(s->status, "Protected item.");
        return;
    }
    sprintf(s->confirm_msg, "Delete %s?", s->items[sel].name);
    s->confirm_action = do_delete;
    s->mode = MODE_CONFIRM;
}

static void menu_sort_name(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    s->sort = SORT_NAME;
    ff_sort(s);
    ff_layout(s);
}
static void menu_sort_date(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    s->sort = SORT_DATE;
    ff_sort(s);
    ff_layout(s);
}
static void menu_sort_size(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    s->sort = SORT_SIZE;
    ff_sort(s);
    ff_layout(s);
}
static void menu_about_filef(void) {
    modal_show(MODAL_INFO, "About FileF", "FileF v2.0\nASMOS File Manager",
               NULL, NULL);
}
static bool ff_close_cb(window *w) {
    ff_inst_t *s = inst_of(w);
    if (!s)
        return true;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (running_apps[i].running && running_apps[i].desc == &filef_app &&
            running_apps[i].state == s) {
            if (s_last_active == s)
                s_last_active = NULL;
            os_quit_app(&running_apps[i]);
            return true;
        }
    }
    return true;
}
static void menu_close(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    ff_close_cb(s->win);
}

/* ── New name / rename ──────────────────────────────────────────────────── */
static void ff_handle_newname(ff_inst_t *s) {
    const char *err = NULL;
    if (!validate_fat_name(s->newname_buf, s->newname_is_dir, &err)) {
        s->mode = MODE_NORMAL;
        modal_show(MODAL_ERROR, "Invalid Name", err, NULL, NULL);
        return;
    }
    char fspath[270];
    item_fspath(s, s->newname_buf, fspath, sizeof(fspath));
    FF_PUSH_CTX(s);
    bool ok;
    if (s->newname_is_dir) {
        ok = fs_mkdir(fspath);
    } else {
        fs_file_t f;
        ok = fs_create(fspath, &f);
        if (ok)
            fs_close(&f);
    }
    if (ok && s->newname_hidden)
        fs_set_hidden(fspath, true);
    FF_POP_CTX();
    if (!ok) {
        s->mode = MODE_NORMAL;
        modal_show(MODAL_ERROR, "Create Failed", "Name exists or disk full.",
                   NULL, NULL);
        return;
    }
    strcpy(s->status, s->newname_is_dir ? "Folder created." : "File created.");
    s->mode = MODE_NORMAL;
    maybe_notify_desktop(s);
    ff_reload(s);
}

static void ff_handle_rename(ff_inst_t *s) {
    const char *err = NULL;
    bool is_dir = s->rename_idx >= 0 &&
                  (s->items[s->rename_idx].entry.attr & ATTR_DIRECTORY);
    if (!validate_fat_name(s->rename_buf, is_dir, &err)) {
        s->mode = MODE_NORMAL;
        modal_show(MODAL_ERROR, "Invalid Name", err, NULL, NULL);
        return;
    }
    char old_fspath[270], new_fspath[270];
    item_fspath(s, s->items[s->rename_idx].name, old_fspath,
                sizeof(old_fspath));
    item_fspath(s, s->rename_buf, new_fspath, sizeof(new_fspath));
    FF_PUSH_CTX(s);
    bool ok = fs_rename(old_fspath, s->rename_buf);
    FF_POP_CTX();
    if (!ok) {
        s->mode = MODE_NORMAL;
        modal_show(MODAL_ERROR, "Rename Failed", "Name exists or protected.",
                   NULL, NULL);
        return;
    }
    strcpy(s->status, "Renamed.");
    s->mode = MODE_NORMAL;
    maybe_notify_desktop(s);
    ff_reload(s);
}

/* ── Drag-and-drop ──────────────────────────────────────────────────────── */
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

static bool is_ancestor_of(uint8_t drive_id, uint16_t src_cluster,
                           uint16_t candidate_cluster) {
    if (src_cluster == candidate_cluster)
        return true;
    uint16_t cur = candidate_cluster;
    for (int depth = 0; cur != 0 && depth < 64; depth++) {
        if (cur == src_cluster)
            return true;
        char name83[12];
        fs_make_83("..", name83);
        dir_entry_t dotdot;
        if (!fs_find_in_dir(drive_id, cur, name83, &dotdot))
            break;
        uint16_t parent = dotdot.cluster_lo;
        if (parent == cur)
            break;
        cur = parent;
    }
    return false;
}

static bool ff_drop_move(ff_inst_t *src, int drag_idx, ff_inst_t *dst) {
    ff_item_t *it = &src->items[drag_idx];
    if (it->is_dotdot || it->is_vdrive)
        return false;
    if (src->drive_id == dst->drive_id && src->dir_cluster == dst->dir_cluster)
        return false;
    if (path_is_protected(it->name)) {
        strcpy(src->status, "Protected item.");
        ff_reload(src);
        return false;
    }
    if ((it->entry.attr & ATTR_DIRECTORY) && src->drive_id == dst->drive_id) {
        if (is_ancestor_of(src->drive_id, it->entry.cluster_lo,
                           dst->dir_cluster)) {
            strcpy(src->status, "Can't move into subfolder.");
            ff_reload(src);
            ff_reload(dst);
            return false;
        }
    }

    char src_fspath[270], dst_fspath[270];
    item_fspath(src, it->name, src_fspath, sizeof(src_fspath));
    item_fspath(dst, it->name, dst_fspath, sizeof(dst_fspath));

    bool ok;
    if (src->drive_id == dst->drive_id) {
        FF_PUSH_CTX(dst);
        ok = (it->entry.attr & ATTR_DIRECTORY)
                 ? fs_move_dir(src_fspath, dst_fspath)
                 : fs_move_file(src_fspath, dst_fspath);
        FF_POP_CTX();
    } else {
        /* cross-drive: copy then delete */
        uint16_t sc = dir_context.current_cluster;
        uint8_t sd = dir_context.drive_id;
        dir_context.current_cluster = dst->dir_cluster;
        dir_context.drive_id = dst->drive_id;
        ok = (it->entry.attr & ATTR_DIRECTORY)
                 ? fs_copy_dir(src_fspath, dst_fspath)
                 : fs_copy_file(src_fspath, dst_fspath);
        dir_context.current_cluster = sc;
        dir_context.drive_id = sd;
        if (ok) {
            uint16_t sc2 = dir_context.current_cluster;
            uint8_t sd2 = dir_context.drive_id;
            dir_context.current_cluster = src->dir_cluster;
            dir_context.drive_id = src->drive_id;
            if (it->entry.attr & ATTR_DIRECTORY)
                fs_rm_rf(src_fspath);
            else
                fs_delete(src_fspath);
            dir_context.current_cluster = sc2;
            dir_context.drive_id = sd2;
        }
    }

    if (ok) {
        strcpy(src->status, "Moved.");
        maybe_notify_desktop(src);
        maybe_notify_desktop(dst);
    } else {
        strcpy(src->status, "Move failed.");
    }
    ff_reload(src);
    ff_reload(dst);
    return ok;
}

static bool ff_drop_to_desktop(ff_inst_t *src, int drag_idx) {
    ff_item_t *it = &src->items[drag_idx];
    if (it->is_dotdot || it->is_vdrive)
        return false;
    if (path_is_protected(it->name)) {
        strcpy(src->status, "Protected.");
        return false;
    }
    char src_fspath[270];
    item_fspath(src, it->name, src_fspath, sizeof(src_fspath));
    /* desktop_accept_drop expects the full logical path */
    char src_logical[270];
    snprintf(src_logical, sizeof(src_logical), "%s/%s", src->path, it->name);
    bool ok = desktop_accept_drop(src_logical, it->name, src->drive_id);
    strcpy(src->status, ok ? "Moved to Desktop." : "Drop failed.");
    ff_reload(src);
    return ok;
}

/* ── Scroll ─────────────────────────────────────────────────────────────── */
static void ff_scroll(ff_inst_t *s, int delta) {
    int view_h = content_view_h(s);
    int ms = s->content_h_total - view_h;
    if (ms < 0)
        ms = 0;
    s->scroll_y += delta;
    if (s->scroll_y < 0)
        s->scroll_y = 0;
    if (s->scroll_y > ms)
        s->scroll_y = ms;
}

/* ── on_frame ───────────────────────────────────────────────────────────── */
static void ff_on_frame(void *state) {
    ff_inst_t *s = (ff_inst_t *)state;
    if (!s->win || !s->win->visible)
        return;

    if (wm_focused_window() == s->win)
        s_last_active = s;

    /* Resize relayout */
    if (s->win->w != s->last_win_w || s->win->h != s->last_win_h) {
        s->last_win_w = s->win->w;
        s->last_win_h = s->win->h;
        ff_layout(s);
        int ms = s->content_h_total - content_view_h(s);
        if (ms < 0)
            ms = 0;
        if (s->scroll_y > ms)
            s->scroll_y = ms;
    }

    /* Auto-refresh if showing desktop folder */
    if (!s->at_vroot && desktop_fs_is_dirty()) {
        char dp[270];
        build_vpath(DRIVE_HDA, "/DESKTOP", dp, sizeof(dp));
        if (strcmp(s->path, dp) == 0) {
            ff_reload(s);
        }
    }

    int wx = s->win->x + 1, wy_top = s->win->y + MENUBAR_H + 16,
        wh_cont = content_view_h(s);
    int conf_by = wy_top + wh_cont / 2 - 16;

    /* Scrollbar drag */
    {
        int ww = s->win->w - 2, wy = s->win->y + MENUBAR_H + 16;
        int sb_x = wx + ww - SCROLLBAR_W - 1, sb_y = wy, sb_h = wh_cont;
        if ((mouse.left_clicked || mouse.left) && mouse.x >= sb_x &&
            mouse.x < sb_x + SCROLLBAR_W + 1 && mouse.y >= sb_y &&
            mouse.y < sb_y + sb_h && s->content_h_total > wh_cont) {
            int ms = s->content_h_total - wh_cont;
            int ns = ((mouse.y - sb_y) * ms) / sb_h;
            if (ns < 0)
                ns = 0;
            if (ns > ms)
                ns = ms;
            s->scroll_y = ns;
        }
    }

    /* Keyboard scroll */
    if (kb.key_pressed) {
        if (kb.last_scancode == F5)
            ff_scroll(s, -wh_cont);
        if (kb.last_scancode == F6)
            ff_scroll(s, wh_cont);
    }

    /* Info overlay */
    if (s->mode == MODE_INFO) {
        if (mouse.left_clicked) {
            int wx2 = s->win->x + 1, wy2 = s->win->y + MENUBAR_H + 16,
                ww2 = s->win->w - 2, wh2 = s->win->h - 16;
            int pw = ww2 - 16, ph = wh2 - 20, px = wx2 + 8, py = wy2 + 10;
            if (mouse.x >= px + pw - 14 && mouse.x < px + pw - 4 &&
                mouse.y >= py + 2 && mouse.y < py + 10)
                s->mode = MODE_NORMAL;
            int bx = px + pw / 2 - 20, by = py + ph - 14;
            if (mouse.x >= bx && mouse.x < bx + 40 && mouse.y >= by &&
                mouse.y < by + 10)
                s->mode = MODE_NORMAL;
        }
        ff_draw_window(s->win, s);
        return;
    }

    /* Newname / rename */
    if (s->mode == MODE_NEWNAME || s->mode == MODE_RENAME) {
        if (kb.key_pressed) {
            char *buf =
                s->mode == MODE_NEWNAME ? s->newname_buf : s->rename_buf;
            int *len =
                s->mode == MODE_NEWNAME ? &s->newname_len : &s->rename_len;
            if (kb.last_scancode == ENTER && *len > 0) {
                if (s->mode == MODE_NEWNAME)
                    ff_handle_newname(s);
                else
                    ff_handle_rename(s);
            } else if (kb.last_scancode == ESC) {
                s->mode = MODE_NORMAL;
            } else if (kb.last_scancode == BACKSPACE) {
                if (*len > 0)
                    buf[--(*len)] = '\0';
            } else if (kb.last_char >= 32 && kb.last_char < 127 && *len < 12) {
                char c = kb.last_char;
                bool allow =
                    fat_char_ok(c) ||
                    (c == '.' && s->mode == MODE_NEWNAME && !s->newname_is_dir);
                if (allow) {
                    buf[(*len)++] = c;
                    buf[*len] = '\0';
                }
            }
        }
        if (s->mode == MODE_NEWNAME && mouse.left_clicked) {
            int wx2 = s->win->x + 1, wy2 = s->win->y + MENUBAR_H + 16,
                ww2 = s->win->w - 2, wh2 = s->win->h - 16;
            int bx = wx2 + 4, by = wy2 + (wh2 - 16) / 2 - 16;
            if (mouse.x >= bx + 4 && mouse.x < bx + 14 && mouse.y >= by + 28 &&
                mouse.y < by + 38)
                s->newname_hidden = !s->newname_hidden;
        }
        ff_draw_window(s->win, s);
        return;
    }

    /* Confirm dialog */
    if (s->mode == MODE_CONFIRM) {
        if (mouse.left_clicked) {
            int bx = wx + 4, by = conf_by;
            if (mouse.x >= bx + 4 && mouse.x < bx + 32 && mouse.y >= by + 20 &&
                mouse.y < by + 30) {
                if (s->confirm_action)
                    s->confirm_action(s);
            } else if (mouse.x >= bx + 36 && mouse.x < bx + 64 &&
                       mouse.y >= by + 20 && mouse.y < by + 30) {
                cancel_confirm(s);
            }
        }
        ff_draw_window(s->win, s);
        return;
    }

    /* Drag in progress */
    if (s->drag_idx >= 0) {
        ff_item_t *it = &s->items[s->drag_idx];
        if (mouse.left) {
            it->icon_x = mouse.x - it->drag_off_x - wx;
            it->icon_y = mouse.y - it->drag_off_y - wy_top + s->scroll_y;
        } else {
            /* Drop */
            bool handled = false;
            if (!in_content(s, mouse.x, mouse.y)) {
                ff_inst_t *dst = ff_find_target(mouse.x, mouse.y, s);
                if (dst) {
                    ff_drop_move(s, s->drag_idx, dst);
                    handled = true;
                } else if (over_desktop(mouse.x, mouse.y)) {
                    ff_drop_to_desktop(s, s->drag_idx);
                    handled = true;
                }
            }
            if (!handled) {
                it->dragging = false;
                it->pinned = false;
                ff_layout(s);
            }
            s->drag_idx = -1;
        }
        ff_draw_window(s->win, s);
        return;
    }

    /* Begin drag */
    if (mouse.left && !mouse.left_clicked && s->drag_idx < 0 &&
        wm_focused_window() == s->win && !s->at_vroot) {
        for (int i = 0; i < s->item_count; i++) {
            ff_item_t *it = &s->items[i];
            if (!it->selected || it->is_dotdot || it->is_vdrive)
                continue;
            if (path_is_protected(it->name))
                continue;
            int ax = wx + it->icon_x + (ICON_CELL_W - ICON_SZ_W) / 2;
            int ay = wy_top + it->icon_y - s->scroll_y;
            if (mouse.x >= ax && mouse.x < ax + ICON_SZ_W && mouse.y >= ay &&
                mouse.y < ay + ICON_SZ_H) {
                if (mouse.dx != 0 || mouse.dy != 0) {
                    it->dragging = true;
                    it->drag_off_x = mouse.x - (ax - ICON_SZ_W / 2);
                    it->drag_off_y = mouse.y - ay;
                    s->drag_idx = i;
                    s->last_click_idx = -1;
                }
                break;
            }
        }
    }

    /* Click / double-click */
    if (mouse.left_clicked) {
        int hit = ff_hit(s, mouse.x, mouse.y);
        if (hit >= 0) {
            for (int i = 0; i < s->item_count; i++)
                s->items[i].selected = false;
            s->items[hit].selected = true;
            uint32_t now = pit_ticks_func();
            if (hit == s->last_click_idx && (now - s->last_click_tick) <= 60) {
                ff_item_t *it = &s->items[hit];

                /* --- Virtual drive entry: enter it --- */
                if (it->is_vdrive) {
                    if (fs_drive_mounted(it->vdrive_id)) {
                        ff_navigate(s, 0, it->vdrive_path, it->vdrive_id);
                    } else {
                        modal_show(MODAL_ERROR, "Drive", "Drive not mounted.",
                                   NULL, NULL);
                    }
                }
                /* --- Dotdot --- */
                else if (it->is_dotdot) {
                    /* Are we at a drive root? → go to vroot */
                    uint8_t drv;
                    char sub[256];
                    bool has_vp =
                        resolve_vpath(s->path, &drv, sub, sizeof(sub));
                    if (has_vp && strcmp(sub, "/") == 0) {
                        ff_go_vroot(s);
                    } else {
                        /* Go up one directory */
                        char parent_logical[256];
                        char *sl = strrchr(s->path, '/');
                        if (sl && sl != s->path) {
                            int plen = (int)(sl - s->path);
                            strncpy(parent_logical, s->path, plen);
                            parent_logical[plen] = '\0';
                        } else {
                            strcpy(parent_logical, "/");
                        }
                        uint8_t pdrv;
                        uint16_t pcluster;
                        if (!is_vroot_path(parent_logical) &&
                            resolve_dir(parent_logical, &pdrv, &pcluster)) {
                            ff_navigate(s, pcluster, parent_logical, pdrv);
                        } else {
                            ff_go_vroot(s);
                        }
                    }
                }
                /* --- App shortcut --- */
                else if (item_is_app(it)) {
                    char app_name[9];
                    int ai = 0;
                    for (int k = 0; it->name[k] && it->name[k] != '.' && ai < 8;
                         k++)
                        app_name[ai++] = it->name[k];
                    app_name[ai] = '\0';
                    app_descriptor *desc = os_find_app(app_name);
                    if (!desc) {
                        char low[9];
                        for (int k = 0; k < ai; k++) {
                            char c = app_name[k];
                            low[k] = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
                        }
                        low[ai] = '\0';
                        desc = os_find_app(low);
                    }
                    if (desc)
                        os_launch_app(desc);
                    else {
                        char em[64];
                        sprintf(em, "App '%s' not found.", app_name);
                        modal_show(MODAL_ERROR, "App Not Found", em, NULL,
                                   NULL);
                    }
                }
                /* --- Directory --- */
                else if (it->entry.attr & ATTR_DIRECTORY) {
                    char child_logical[270];
                    snprintf(child_logical, sizeof(child_logical), "%s/%s",
                             s->path, it->name);
                    uint16_t child_cluster = it->entry.cluster_lo;
                    if (g_cfg.filef_single_window)
                        ff_navigate(s, child_cluster, child_logical,
                                    s->drive_id);
                    else
                        ff_open_dir_pub(child_cluster, child_logical);
                }

                s->last_click_idx = -1;
                s->last_click_tick = 0;
            } else {
                s->last_click_idx = hit;
                s->last_click_tick = pit_ticks_func();
            }
        } else if (in_content(s, mouse.x, mouse.y)) {
            for (int i = 0; i < s->item_count; i++)
                s->items[i].selected = false;
        }
    }

    ff_draw_window(s->win, s);
}

/* ── Init / destroy ─────────────────────────────────────────────────────── */
static void ff_init(void *state) {
    ff_inst_t *s = (ff_inst_t *)state;
    s->scroll_y = 0;
    s->last_win_w = FF_DEFAULT_W;
    s->last_win_h = FF_DEFAULT_H;
    s->show_hidden = false;
    s->at_vroot = true;

    int x = FF_DEFAULT_X + (s_cascade % 6) * FF_CASCADE;
    int y = FF_DEFAULT_Y + (s_cascade % 6) * FF_CASCADE;
    s_cascade++;

    const window_spec_t spec = {
        .x = x,
        .y = y,
        .w = FF_DEFAULT_W,
        .h = FF_DEFAULT_H,
        .min_w = 80,
        .min_h = 80,
        .resizable = true,
        .title = "FileF",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = WHITE,
        .visible = true,
        .on_close = ff_close_cb,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;
    s->win->on_draw = ff_draw_window;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "New File", menu_new_file);
    menu_add_item(file_menu, "New Folder", menu_new_folder);
    menu_add_item(file_menu, "Rename", menu_rename);
    menu_add_item(file_menu, "Get Info", menu_get_info);
    menu_add_item(file_menu, "Reload", menu_reload);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "About FileF", menu_about_filef);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", menu_close);

    menu *edit_menu = window_add_menu(s->win, "Edit");
    menu_add_item(edit_menu, "Copy", menu_copy);
    menu_add_item(edit_menu, "Cut", menu_cut);
    menu_add_item(edit_menu, "Paste", menu_paste);
    menu_add_separator(edit_menu);
    menu_add_item(edit_menu, "Delete", menu_delete);
    menu_add_separator(edit_menu);
    menu_add_item(edit_menu, "Toggle Hidden", menu_toggle_hidden);

    menu *view_menu = window_add_menu(s->win, "View");
    menu_add_item(view_menu, "By Name", menu_sort_name);
    menu_add_item(view_menu, "By Date", menu_sort_date);
    menu_add_item(view_menu, "By Size", menu_sort_size);
    menu_add_separator(view_menu);
    menu_add_item(view_menu, "Show Hidden", menu_show_hidden_toggle);

    s->dir_cluster = 0;
    s->drive_id = DRIVE_HDA;
    s->sort = SORT_NAME;
    s->drag_idx = -1;
    s->last_click_idx = -1;
    s->mode = MODE_NORMAL;
    strcpy(s->path, "/");
    s->win->title = s->path;
    ff_reload(s); /* shows vroot */

    s_last_active = s;
}

static void ff_destroy(void *state) {
    ff_inst_t *s = (ff_inst_t *)state;
    if (s_last_active == s)
        s_last_active = NULL;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor filef_app = {
    .name = "FILEF",
    .state_size = sizeof(ff_inst_t),
    .init = ff_init,
    .on_frame = ff_on_frame,
    .destroy = ff_destroy,
};
