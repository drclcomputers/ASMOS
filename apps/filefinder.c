#include "os/api.h"

#define FF_DEFAULT_W    200
#define FF_DEFAULT_H    150
#define FF_DEFAULT_X    40
#define FF_DEFAULT_Y    20
#define FF_CASCADE      16

#define ICON_SZ_W       20
#define ICON_SZ_H       20
#define ICON_CELL_W     44
#define ICON_CELL_H     30
#define CONTENT_TOP     6
#define CONTENT_BOT     10
#define STATUS_H        8
#define CHAR_W          5
#define CHAR_H          6
#define LABEL_CHARS     8

#define MAX_ENTRIES     64
#define SORT_NAME       0
#define SORT_DATE       1
#define SORT_SIZE       2
#define MODE_NORMAL     0
#define MODE_RENAME     1
#define MODE_CONFIRM    2
#define MODE_NEWNAME    3

typedef struct {
    dir_entry_t entry;
    char        name[13];
    char        label[LABEL_CHARS + 1];
    bool        is_dotdot;
    int         icon_x, icon_y;
    bool        selected;
    bool        dragging;
    int         drag_off_x, drag_off_y;
    bool        pinned;
} ff_item_t;

typedef struct {
    window     *win;
    uint16_t    dir_cluster;
    char        path[256];

    ff_item_t   items[MAX_ENTRIES];
    int         item_count;

    int         sort;
    int         mode;

    char        rename_buf[13];
    int         rename_len;
    int         rename_idx;

    char        newname_buf[13];
    int         newname_len;
    bool        newname_is_dir;

    char        confirm_msg[80];
    void      (*confirm_action)(void *inst_state);

    int         drag_idx;
    uint32_t    last_click_tick;
    int         last_click_idx;

    char        status[48];
} ff_inst_t;

typedef struct {
    char  path[256];
    char  name[13];
    bool  is_dir;
    bool  is_cut;
    bool  valid;
} ff_clip_t;

static ff_clip_t g_clip = { .valid = false };

app_descriptor finder_app;

static int  s_cascade = 0;

static void ff_reload(ff_inst_t *s);
static void ff_draw_window(window *win, void *ud);
static void ff_open_dir(uint16_t cluster, const char *path);

static void menu_new_file(void);
static void menu_new_folder(void);
static void menu_rename(void);
static void menu_reload(void);
static void menu_close(void);
static void menu_copy(void);
static void menu_cut(void);
static void menu_paste(void);
static void menu_delete(void);
static void menu_sort_name(void);
static void menu_sort_date(void);
static void menu_sort_size(void);
static void cancel_confirm(void *ud);
static int  ff_hit(ff_inst_t *s, int mx, int my);
static bool in_content(ff_inst_t *s, int mx, int my);

static ff_inst_t *inst_of(window *w) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &finder_app) continue;
        ff_inst_t *s = (ff_inst_t *)a->state;
        if (s->win == w) return s;
    }
    return NULL;
}

static ff_inst_t *active_finder(void) {
    window *fw = wm_focused_window();
    if (!fw) return NULL;
    return inst_of(fw);
}

static void entry_to_name(const dir_entry_t *e, char *out) {
    int j = 0;
    for (int k = 0; k < 8 && e->name[k] != ' '; k++) out[j++] = e->name[k];
    if (!(e->attr & ATTR_DIRECTORY) && e->name[0] != '.' && e->ext[0] != ' ') {
        out[j++] = '.';
        for (int k = 0; k < 3 && e->ext[k] != ' '; k++) out[j++] = e->ext[k];
    }
    out[j] = '\0';
}

static void ff_make_label(const char *name, char *out) {
    int i = 0;
    while (name[i] && i < LABEL_CHARS) { out[i] = name[i]; i++; }
    out[i] = '\0';
}

static bool fat_char_ok(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= '0' && c <= '9') return true;
    const char *ok = "!#$%&'()-@^_`{}~";
    for (int i = 0; ok[i]; i++) if (c == ok[i]) return true;
    return false;
}

static bool validate_fat_name(const char *name_buf, bool is_dir, const char **err_msg) {
    if (!name_buf || name_buf[0] == '\0') {
        *err_msg = "Name cannot be empty.";
        return false;
    }

    const char *dot = strchr(name_buf, '.');
    int base_len, ext_len;

    if (dot) {
        base_len = (int)(dot - name_buf);
        ext_len  = (int)strlen(dot + 1);
        if (is_dir && ext_len > 0) {
            *err_msg = "Folder name: no extension allowed.";
            return false;
        }
        if (ext_len > 3) {
            *err_msg = "Extension too long (max 3).";
            return false;
        }
        for (int i = 0; i < ext_len; i++) {
            if (!fat_char_ok(dot[1 + i])) {
                *err_msg = "Invalid char in extension.";
                return false;
            }
        }
    } else {
        base_len = (int)strlen(name_buf);
        ext_len  = 0;
    }

    if (base_len == 0) {
        *err_msg = "Base name cannot be empty.";
        return false;
    }
    if (base_len > 8) {
        *err_msg = "Base name too long (max 8).";
        return false;
    }

    for (int i = 0; i < base_len; i++) {
        if (!fat_char_ok(name_buf[i])) {
            *err_msg = "Invalid character in name.";
            return false;
        }
    }

    (void)ext_len;
    return true;
}

static int item_sort_cmp(const ff_item_t *a, const ff_item_t *b, int sort) {
    if (a->is_dotdot) return -1;
    if (b->is_dotdot) return  1;
    bool ad = (a->entry.attr & ATTR_DIRECTORY) != 0;
    bool bd = (b->entry.attr & ATTR_DIRECTORY) != 0;
    if (ad != bd) return ad ? -1 : 1;
    if (sort == SORT_SIZE)
        return (int)a->entry.file_size - (int)b->entry.file_size;
    if (sort == SORT_DATE) {
        int dc = (int)b->entry.modify_date - (int)a->entry.modify_date;
        if (dc != 0) return dc;
        return (int)b->entry.modify_time - (int)a->entry.modify_time;
    }
    return strcmp(a->name, b->name);
}

static void ff_sort(ff_inst_t *s) {
    for (int i = 0; i < s->item_count - 1; i++)
        for (int j = i + 1; j < s->item_count; j++)
            if (item_sort_cmp(&s->items[i], &s->items[j], s->sort) > 0) {
                ff_item_t tmp = s->items[i];
                s->items[i]   = s->items[j];
                s->items[j]   = tmp;
            }
}

static void ff_layout(ff_inst_t *s) {
    int ww   = s->win->w - 2;
    int cols = (ww - 4) / ICON_CELL_W;
    if (cols < 1) cols = 1;
    int col = 0, row = 0;
    for (int i = 0; i < s->item_count; i++) {
        ff_item_t *it = &s->items[i];
        if (it->pinned) continue;
        if (it->dragging) continue;
        it->icon_x = 4 + col * ICON_CELL_W;
        it->icon_y = CONTENT_TOP + row * ICON_CELL_H;
        col++;
        if (col >= cols) { col = 0; row++; }
    }
}

static void ff_reload(ff_inst_t *s) {
    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s->dir_cluster;

    dir_entry_t raw[MAX_ENTRIES];
    int count = 0;
    fat16_list_dir(s->dir_cluster, raw, MAX_ENTRIES, &count);

    s->item_count = 0;

    if (s->dir_cluster != 0) {
        ff_item_t *up = &s->items[s->item_count++];
        memset(up, 0, sizeof(ff_item_t));
        up->is_dotdot = true;
        strcpy(up->name, "..");
        strcpy(up->label, "..");
    }

    for (int i = 0; i < count && s->item_count < MAX_ENTRIES; i++) {
        if (raw[i].name[0] == '.') continue;
        ff_item_t *it = &s->items[s->item_count++];
        memset(it, 0, sizeof(ff_item_t));
        it->entry = raw[i];
        entry_to_name(&raw[i], it->name);
        ff_make_label(it->name, it->label);
    }

    dir_context.current_cluster = saved;

    ff_sort(s);
    ff_layout(s);
    s->drag_idx = -1;
    s->status[0] = '\0';
}

static void draw_folder_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax,     ay + 3,  ICON_SZ_W,     ICON_SZ_H - 3, bg);
    draw_rect(ax,     ay + 3,  ICON_SZ_W,     ICON_SZ_H - 3, BLACK);
    fill_rect(ax + 1, ay,      8,             4,             bg);
    draw_rect(ax + 1, ay,      8,             4,             BLACK);
    draw_line(ax,     ay + 3,  ax + 1,        ay,            BLACK);
    draw_line(ax + 9, ay,      ax + 9,        ay + 3,        BLACK);
    if (sel) {
        fill_rect(ax + 2, ay + 5, ICON_SZ_W - 4, ICON_SZ_H - 9, BLACK);
    }
}

static void draw_file_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax + 1, ay,      ICON_SZ_W - 5, ICON_SZ_H,    bg);
    draw_rect(ax + 1, ay,      ICON_SZ_W - 5, ICON_SZ_H,    BLACK);
    fill_rect(ax + ICON_SZ_W - 5, ay, 4, ICON_SZ_H,         BLACK);
    draw_line(ax + ICON_SZ_W - 5, ay, ax + ICON_SZ_W - 1, ay + 4, BLACK);
    fill_rect(ax + ICON_SZ_W - 4, ay, 3, 4,                bg);
    if (!sel) {
        draw_line(ax + 3, ay + 5,  ax + ICON_SZ_W - 6, ay + 5,  DARK_GRAY);
        draw_line(ax + 3, ay + 8,  ax + ICON_SZ_W - 6, ay + 8,  DARK_GRAY);
        draw_line(ax + 3, ay + 11, ax + ICON_SZ_W - 6, ay + 11, DARK_GRAY);
    }
}

static void draw_dotdot_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : LIGHT_GRAY;
    fill_rect(ax, ay + 3, ICON_SZ_W, ICON_SZ_H - 3, bg);
    draw_rect(ax, ay + 3, ICON_SZ_W, ICON_SZ_H - 3, BLACK);
    draw_string(ax + 4, ay + 8, "..", BLACK, 2);
}

static void ff_draw_item(ff_inst_t *s, int i, int base_x, int base_y) {
    ff_item_t *it = &s->items[i];
    int ax = base_x + it->icon_x + (ICON_CELL_W - ICON_SZ_W) / 2;
    int ay = base_y + it->icon_y;

    if (it->is_dotdot)
        draw_dotdot_icon(ax, ay, it->selected);
    else if (it->entry.attr & ATTR_DIRECTORY)
        draw_folder_icon(ax, ay, it->selected);
    else
        draw_file_icon(ax, ay, it->selected);

    int lw = (int)strlen(it->label) * CHAR_W;
    int lx = ax + ICON_SZ_W / 2 - lw / 2;
    int ly = ay + ICON_SZ_H + 1;

    if (it->selected) {
        fill_rect(lx - 1, ly - 1, lw + 2, CHAR_H + 2, BLACK);
        draw_string(lx, ly, it->label, WHITE, 2);
    } else {
        draw_string(lx + 1, ly + 1, it->label, WHITE, 2);
        draw_string(lx,     ly,     it->label, BLACK, 2);
    }

    if (s->mode == MODE_RENAME && i == s->rename_idx) {
        fill_rect(ax - 1, ay + ICON_SZ_H + 1 - 1, ICON_CELL_W, CHAR_H + 2, WHITE);
        draw_rect(ax - 1, ay + ICON_SZ_H + 1 - 1, ICON_CELL_W, CHAR_H + 2, BLACK);
        draw_string(ax, ay + ICON_SZ_H + 1, s->rename_buf, BLACK, 2);

        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 50) % 2 == 0) {
            int cx = ax + s->rename_len * CHAR_W;
            draw_string(cx, ay + ICON_SZ_H + 1, "|", BLACK, 2);
        }
    }
}

static void ff_draw_window(window *win, void *ud) {
    ff_inst_t *s = (ff_inst_t *)ud;
    if (!s) return;

    int wx        = win->x + 1;
    int wy        = win->y + MENUBAR_H + 16;
    int ww        = win->w - 2;
    int wh        = win->h - 16;
    int content_y = wy + CONTENT_TOP;
    int content_h = wh - CONTENT_TOP - CONTENT_BOT - STATUS_H;

    fill_rect(wx, wy, ww, wh, WHITE);

    draw_line(wx, wy + CONTENT_TOP - 1,
              wx + ww - 1, wy + CONTENT_TOP - 1, DARK_GRAY);
    draw_line(wx, wy + wh - STATUS_H - CONTENT_BOT,
              wx + ww - 1, wy + wh - STATUS_H - CONTENT_BOT, DARK_GRAY);

    for (int i = 0; i < s->item_count; i++) {
        if (s->items[i].dragging) continue;
        ff_draw_item(s, i, wx, content_y);
    }

    if (s->drag_idx >= 0) {
        ff_item_t *it = &s->items[s->drag_idx];
        int ax = mouse.x - it->drag_off_x;
        int ay = mouse.y - it->drag_off_y;
        int save_x = it->icon_x, save_y = it->icon_y;
        it->icon_x = ax - wx;
        it->icon_y = ay - content_y;
        ff_draw_item(s, s->drag_idx, wx, content_y);
        it->icon_x = save_x;
        it->icon_y = save_y;
    }

    char stat[64];
    if (s->status[0]) {
        strncpy(stat, s->status, 63); stat[63] = '\0';
    } else {
        sprintf(stat, "%d items", s->item_count - (s->dir_cluster != 0 ? 1 : 0));
    }
    draw_string(wx + 3, wy + wh - STATUS_H - CONTENT_BOT + 2, stat, BLACK, 2);

    if (s->mode == MODE_CONFIRM) {
        int bx = wx + 4, by = wy + content_h / 2 - 16;
        int bw = ww - 8;
        fill_rect(bx, by, bw, 34, LIGHT_GRAY);
        draw_rect(bx, by, bw, 34, BLACK);
        draw_string(bx + 4, by + 4, s->confirm_msg, BLACK, 2);
        fill_rect(bx + 4,  by + 20, 28, 10, BLACK);
        draw_rect(bx + 4,  by + 20, 28, 10, BLACK);
        draw_string(bx + 8,  by + 22, "Yes", WHITE, 2);
        fill_rect(bx + 36, by + 20, 28, 10, LIGHT_GRAY);
        draw_rect(bx + 36, by + 20, 28, 10, BLACK);
        draw_string(bx + 40, by + 22, "No",  BLACK, 2);
    }

    if (s->mode == MODE_NEWNAME) {
        int bx = wx + 4, by = wy + content_h / 2 - 16;
        int bw = ww - 8;
        fill_rect(bx, by, bw, 34, LIGHT_GRAY);
        draw_rect(bx, by, bw, 34, BLACK);
        const char *prompt = s->newname_is_dir ? "Folder name:" : "File name:";
        draw_string(bx + 4, by + 4, (char *)prompt, BLACK, 2);

        fill_rect(bx + 4, by + 14, bw - 8, 10, WHITE);
        draw_rect(bx + 4, by + 14, bw - 8, 10, BLACK);
        draw_string(bx + 6, by + 15, s->newname_buf, BLACK, 2);

        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 50) % 2 == 0) {
            int cx = bx + 6 + s->newname_len * CHAR_W;
            int max_cx = bx + bw - 8 - CHAR_W - 2;
            if (cx < max_cx)
                draw_string(cx, by + 15, "|", BLACK, 2);
        }
    }
}

static int ff_hit(ff_inst_t *s, int mx, int my) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16 + CONTENT_TOP;
    for (int i = 0; i < s->item_count; i++) {
        ff_item_t *it = &s->items[i];
        int ax = wx + it->icon_x + (ICON_CELL_W - ICON_SZ_W) / 2;
        int ay = wy + it->icon_y;
        if (mx >= ax && mx < ax + ICON_SZ_W &&
            my >= ay && my < ay + ICON_SZ_H + CHAR_H + 2)
            return i;
    }
    return -1;
}

static bool in_content(ff_inst_t *s, int mx, int my) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16 + CONTENT_TOP;
    int ww = s->win->w - 2;
    int wh = s->win->h - 16 - CONTENT_TOP - CONTENT_BOT - STATUS_H;
    return mx >= wx && mx < wx + ww && my >= wy && my < wy + wh;
}

static void do_delete(void *ud) {
    ff_inst_t *s = (ff_inst_t *)ud;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) { sel = i; break; }
    if (sel < 0) { s->mode = MODE_NORMAL; return; }

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s->dir_cluster;

    ff_item_t *it = &s->items[sel];
    bool ok;
    if (it->entry.attr & ATTR_DIRECTORY)
        ok = fat16_rm_rf(it->name);
    else
        ok = fat16_delete(it->name);

    dir_context.current_cluster = saved;
    strcpy(s->status, ok ? "Deleted." : "Delete failed.");
    s->mode = MODE_NORMAL;
    ff_reload(s);
}

static void cancel_confirm(void *ud) {
    ff_inst_t *s = (ff_inst_t *)ud;
    s->mode = MODE_NORMAL;
}

static void menu_new_file(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    s->newname_buf[0] = '\0';
    s->newname_len    = 0;
    s->newname_is_dir = false;
    s->mode = MODE_NEWNAME;
}

static void menu_new_folder(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    s->newname_buf[0] = '\0';
    s->newname_len    = 0;
    s->newname_is_dir = true;
    s->mode = MODE_NEWNAME;
}

static void menu_rename(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) { sel = i; break; }
    if (sel < 0) { strcpy(s->status, "Nothing selected."); return; }
    s->rename_idx = sel;
    strncpy(s->rename_buf, s->items[sel].name, 12);
    s->rename_buf[12] = '\0';
    s->rename_len = (int)strlen(s->rename_buf);
    s->mode = MODE_RENAME;
}

static void menu_reload(void) {
	ff_inst_t *s = active_finder(); if (!s) return;
	ff_reload(s);
}

static void menu_copy(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) { sel = i; break; }
    if (sel < 0) { strcpy(s->status, "Nothing selected."); return; }
    strncpy(g_clip.name, s->items[sel].name, 12); g_clip.name[12] = '\0';
    strncpy(g_clip.path, s->path, 255);           g_clip.path[255] = '\0';
    g_clip.is_dir = (s->items[sel].entry.attr & ATTR_DIRECTORY) != 0;
    g_clip.is_cut = false;
    g_clip.valid  = true;
    strcpy(s->status, "Copied.");
}

static void menu_cut(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) { sel = i; break; }
    if (sel < 0) { strcpy(s->status, "Nothing selected."); return; }
    strncpy(g_clip.name, s->items[sel].name, 12); g_clip.name[12] = '\0';
    strncpy(g_clip.path, s->path, 255);           g_clip.path[255] = '\0';
    g_clip.is_dir = (s->items[sel].entry.attr & ATTR_DIRECTORY) != 0;
    g_clip.is_cut = true;
    g_clip.valid  = true;
    strcpy(s->status, "Cut.");
}

static void menu_paste(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    if (!g_clip.valid) { strcpy(s->status, "Clipboard empty."); return; }

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s->dir_cluster;

    char src_path[270];
    if (g_clip.path[0] == '/' && g_clip.path[1] == '\0')
        sprintf(src_path, "/%s", g_clip.name);
    else
        sprintf(src_path, "%s/%s", g_clip.path, g_clip.name);

    dir_entry_t de;
    if (fat16_find(g_clip.name, &de)) {
        strcpy(s->status, "Name already exists.");
        dir_context.current_cluster = saved;
        return;
    }

    bool ok;
    if (g_clip.is_cut) {
        ok = g_clip.is_dir ? fat16_move_dir(src_path, g_clip.name)
                           : fat16_move_file(src_path, g_clip.name);
        if (ok) g_clip.valid = false;
    } else {
        ok = g_clip.is_dir ? fat16_copy_dir(src_path, g_clip.name)
                           : fat16_copy_file(src_path, g_clip.name);
    }

    dir_context.current_cluster = saved;
    strcpy(s->status, ok ? "Pasted." : "Paste failed.");
    ff_reload(s);
}

static void menu_delete(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) { sel = i; break; }
    if (sel < 0) { strcpy(s->status, "Nothing selected."); return; }
    sprintf(s->confirm_msg, "Delete %s?", s->items[sel].name);
    s->confirm_action = do_delete;
    s->mode = MODE_CONFIRM;
}

static void menu_sort_name(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    s->sort = SORT_NAME; ff_sort(s); ff_layout(s);
}
static void menu_sort_date(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    s->sort = SORT_DATE; ff_sort(s); ff_layout(s);
}
static void menu_sort_size(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    s->sort = SORT_SIZE; ff_sort(s); ff_layout(s);
}

static bool ff_close_cb(window *w) {
    ff_inst_t *s = inst_of(w);
    if (!s) return true;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (running_apps[i].running &&
            running_apps[i].desc  == &finder_app &&
            running_apps[i].state == s) {
            os_quit_app(&running_apps[i]);
            return true;
        }
    }
    return true;
}

static void menu_close(void) {
    ff_inst_t *s = active_finder(); if (!s) return;
    ff_close_cb(s->win);
}

static void ff_handle_newname(ff_inst_t *s) {
    const char *err = NULL;
    if (!validate_fat_name(s->newname_buf, s->newname_is_dir, &err)) {
        s->mode = MODE_NORMAL;
        modal_show(MODAL_ERROR, "Invalid Name", err, NULL, NULL);
        return;
    }

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s->dir_cluster;
    bool ok;
    if (s->newname_is_dir) {
        ok = fat16_mkdir(s->newname_buf);
    } else {
        fat16_file_t f;
        ok = fat16_create(s->newname_buf, &f);
        if (ok) fat16_close(&f);
    }
    dir_context.current_cluster = saved;

    if (!ok) {
        s->mode = MODE_NORMAL;
        modal_show(MODAL_ERROR, "Create Failed",
                   "Name already exists or disk full.", NULL, NULL);
        return;
    }

    strcpy(s->status, s->newname_is_dir ? "Folder created." : "File created.");
    s->mode = MODE_NORMAL;
    ff_reload(s);
}

static void ff_handle_rename(ff_inst_t *s) {
    const char *err = NULL;
    bool is_dir = (s->rename_idx >= 0) &&
                  (s->items[s->rename_idx].entry.attr & ATTR_DIRECTORY);
    if (!validate_fat_name(s->rename_buf, is_dir, &err)) {
        s->mode = MODE_NORMAL;
        modal_show(MODAL_ERROR, "Invalid Name", err, NULL, NULL);
        return;
    }

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s->dir_cluster;
    bool ok = fat16_rename(s->items[s->rename_idx].name, s->rename_buf);
    dir_context.current_cluster = saved;

    if (!ok) {
        s->mode = MODE_NORMAL;
        modal_show(MODAL_ERROR, "Rename Failed",
                   "Name already exists.", NULL, NULL);
        return;
    }

    strcpy(s->status, "Renamed.");
    s->mode = MODE_NORMAL;
    ff_reload(s);
}

static ff_inst_t *ff_find_target(int mx, int my, ff_inst_t *exclude) {
    for (int i = win_count - 1; i >= 0; i--) {
        window *w = win_stack[i];
        if (!w->visible) continue;

        ff_inst_t *t = inst_of(w);
        if (!t || t == exclude) continue;

        if (in_content(t, mx, my)) return t;
    }
    return NULL;
}


static bool is_ancestor_of(uint16_t src_cluster, uint16_t candidate_cluster) {
    if (src_cluster == candidate_cluster) return true;

    uint16_t cur = candidate_cluster;
    int depth = 0;
    const int MAX_DEPTH = 64;

    while (cur != 0 && depth < MAX_DEPTH) {
        if (cur == src_cluster) return true;

        uint16_t saved = dir_context.current_cluster;
        dir_context.current_cluster = cur;

        dir_entry_t entries[16];
        int count = 0;
        fat16_list_dir(cur, entries, 16, &count);
        dir_context.current_cluster = saved;

        dir_entry_t dotdot;
        char name83[12];
        fat16_make_83("..", name83);
        if (!fat16_find_in_dir(cur, name83, &dotdot)) break;

        uint16_t parent = dotdot.cluster_lo;
        if (parent == cur) break;
        cur = parent;
        depth++;
    }
    return false;
}

static bool ff_drop_move(ff_inst_t *src, int drag_idx, ff_inst_t *dst) {
    ff_item_t *it = &src->items[drag_idx];
    if (it->is_dotdot) return false;
    if (src->dir_cluster == dst->dir_cluster) return false;

    if (it->entry.attr & ATTR_DIRECTORY) {
        if (is_ancestor_of(it->entry.cluster_lo, dst->dir_cluster)) {
        	ff_reload(src);
         	ff_reload(dst);
            strcpy(src->status, "Can't move into subfolder.");
            return false;
        }
    }

    char src_path[270];
    if (src->path[0] == '/' && src->path[1] == '\0')
        sprintf(src_path, "/%s", it->name);
    else
        sprintf(src_path, "%s/%s", src->path, it->name);

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = dst->dir_cluster;

    dir_entry_t de;
    if (fat16_find(it->name, &de)) {
        dir_context.current_cluster = saved;
        strcpy(src->status, "Name exists in target.");
        return false;
    }

    bool ok;
    if (it->entry.attr & ATTR_DIRECTORY)
        ok = fat16_move_dir(src_path, it->name);
    else
        ok = fat16_move_file(src_path, it->name);

    dir_context.current_cluster = saved;

    if (ok) {
        strcpy(src->status, "Moved.");
        strcpy(dst->status, "Item received.");
        ff_reload(src);
        ff_reload(dst);
    } else {
        strcpy(src->status, "Move failed.");
    }
    return ok;
}

static void ff_open_dir(uint16_t cluster, const char *path) {
    app_instance_t *inst = os_launch_app(&finder_app);
    if (!inst) return;
    ff_inst_t *s = (ff_inst_t *)inst->state;
    s->dir_cluster = cluster;
    strncpy(s->path, path, 255); s->path[255] = '\0';
    ff_reload(s);
}

static void ff_on_frame(void *state) {
    ff_inst_t *s = (ff_inst_t *)state;
    if (!s->win || !s->win->visible) return;

    int wx       = s->win->x + 1;
    int wy_top   = s->win->y + MENUBAR_H + 16 + CONTENT_TOP;
    int wh_cont  = s->win->h - 16 - CONTENT_TOP - CONTENT_BOT - STATUS_H;
    int conf_by  = wy_top + wh_cont / 2 - 16;

    if (s->mode == MODE_NEWNAME || s->mode == MODE_RENAME) {
        if (kb.key_pressed) {
            char *buf = (s->mode == MODE_NEWNAME) ? s->newname_buf : s->rename_buf;
            int  *len = (s->mode == MODE_NEWNAME) ? &s->newname_len : &s->rename_len;
            int   max = 12;

            if (kb.last_scancode == ENTER && *len > 0) {
                if (s->mode == MODE_NEWNAME) ff_handle_newname(s);
                else                         ff_handle_rename(s);
            } else if (kb.last_scancode == ESC) {
                s->mode = MODE_NORMAL;
            } else if (kb.last_scancode == BACKSPACE) {
                if (*len > 0) buf[--(*len)] = '\0';
            } else if (kb.last_char >= 32 && kb.last_char < 127 && *len < max) {
                char c = kb.last_char;
                bool allow = fat_char_ok(c) ||
                             (c == '.' && s->mode == MODE_NEWNAME && !s->newname_is_dir);
                if (allow) {
                    buf[(*len)++] = c;
                    buf[*len]     = '\0';
                }
            }
        }
        ff_draw_window(s->win, s);
        return;
    }

    if (s->mode == MODE_CONFIRM) {
        if (mouse.left_clicked) {
            int bx = wx + 4;
            int by = conf_by;
            int bw = s->win->w - 10;
            (void)bw;
            if (mouse.x >= bx + 4  && mouse.x < bx + 32  &&
                mouse.y >= by + 20 && mouse.y < by + 30) {
                if (s->confirm_action) s->confirm_action(s);
            } else if (mouse.x >= bx + 36 && mouse.x < bx + 64 &&
                       mouse.y >= by + 20 && mouse.y < by + 30) {
                cancel_confirm(s);
            }
        }
        ff_draw_window(s->win, s);
        return;
    }


    if (s->drag_idx >= 0) {
        ff_item_t *it = &s->items[s->drag_idx];
        if (mouse.left) {
            int new_ix = mouse.x - it->drag_off_x - wx;
            int new_iy = mouse.y - it->drag_off_y - wy_top;
            int max_x  = s->win->w - 2 - ICON_SZ_W;
            int max_y  = wh_cont - ICON_SZ_H - CHAR_H - 4;
            if (new_ix < 0) new_ix = 0;
            if (new_iy < 0) new_iy = 0;
            if (new_ix > max_x) new_ix = max_x;
            if (new_iy > max_y) new_iy = max_y;
            it->icon_x = new_ix;
            it->icon_y = new_iy;
        } else {
            if (!in_content(s, mouse.x, mouse.y)) {
                ff_inst_t *dst = ff_find_target(mouse.x, mouse.y, s);
                if (dst) {
                    ff_drop_move(s, s->drag_idx, dst);
                    s->drag_idx = -1;
                } else {
                    it->dragging = false;
                    it->pinned   = false;
                    s->drag_idx  = -1;
                    ff_layout(s);
                }
            } else {
                it->dragging = false;
                it->pinned   = true;
                s->drag_idx  = -1;
            }
        }
        ff_draw_window(s->win, s);
        return;
    }

    if (mouse.left && !mouse.left_clicked && s->drag_idx < 0) {
        for (int i = 0; i < s->item_count; i++) {
            ff_item_t *it = &s->items[i];
            if (!it->selected || it->is_dotdot) continue;
            int ax = wx + it->icon_x + (ICON_CELL_W - ICON_SZ_W) / 2;
            int ay = wy_top + it->icon_y;
            if (mouse.x >= ax && mouse.x < ax + ICON_SZ_W &&
                mouse.y >= ay && mouse.y < ay + ICON_SZ_H) {
	                if (mouse.dx != 0 || mouse.dy != 0) {
	                    it->dragging   = true;
	                    it->drag_off_x = mouse.x - (ax - ICON_SZ_W / 2);
	                    it->drag_off_y = mouse.y - ay;
	                    s->drag_idx    = i;
	                    s->last_click_idx = -1;
                }
                break;
            }
        }
    }

    if (mouse.left_clicked) {
        int hit = ff_hit(s, mouse.x, mouse.y);
        if (hit >= 0) {
            for (int i = 0; i < s->item_count; i++) s->items[i].selected = false;
            s->items[hit].selected = true;

            uint32_t now = pit_ticks_func();
            if (hit == s->last_click_idx && (now - s->last_click_tick) <= 60) {
                ff_item_t *it = &s->items[hit];
                if (it->is_dotdot) {
                    char parent[256];
                    char *slash = strrchr(s->path, '/');
                    if (slash && slash != s->path) {
                        int len = (int)(slash - s->path);
                        strncpy(parent, s->path, len); parent[len] = '\0';
                    } else {
                        strcpy(parent, "/");
                    }
                    uint16_t saved = dir_context.current_cluster;
                    dir_context.current_cluster = s->dir_cluster;
                    fat16_chdir("..");
                    uint16_t parent_cluster = dir_context.current_cluster;
                    dir_context.current_cluster = saved;
                    ff_open_dir(parent_cluster, parent);
                } else if (it->entry.attr & ATTR_DIRECTORY) {
                    char child_path[270];
                    if (s->path[0] == '/' && s->path[1] == '\0')
                        sprintf(child_path, "/%s", it->name);
                    else
                        sprintf(child_path, "%s/%s", s->path, it->name);
                    ff_open_dir(it->entry.cluster_lo, child_path);
                }
                s->last_click_idx  = -1;
                s->last_click_tick = 0;
            } else {
                s->last_click_idx  = hit;
                s->last_click_tick = pit_ticks_func();
            }
        } else if (in_content(s, mouse.x, mouse.y)) {
            for (int i = 0; i < s->item_count; i++) s->items[i].selected = false;
        }
    }

    ff_draw_window(s->win, s);
}

static void ff_init(void *state) {
    ff_inst_t *s = (ff_inst_t *)state;

    int x = FF_DEFAULT_X + (s_cascade % 6) * FF_CASCADE;
    int y = FF_DEFAULT_Y + (s_cascade % 6) * FF_CASCADE;
    s_cascade++;

    const window_spec_t spec = {
        .x             = x,
        .y             = y,
        .w             = FF_DEFAULT_W,
        .h             = FF_DEFAULT_H,
        .resizable     = true,
        .title         = "Finder",
        .title_color   = WHITE,
        .bar_color     = DARK_GRAY,
        .content_color = WHITE,
        .visible       = true,
        .on_close      = ff_close_cb,
    };
    s->win = wm_register(&spec);
    if (!s->win) return;

    s->win->on_draw          = ff_draw_window;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "New File",   menu_new_file);
    menu_add_item(file_menu, "New Folder", menu_new_folder);
    menu_add_item(file_menu, "Rename",     menu_rename);
    menu_add_item(file_menu, "Reload",     menu_reload);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close",      menu_close);

    menu *edit_menu = window_add_menu(s->win, "Edit");
    menu_add_item(edit_menu, "Copy",   menu_copy);
    menu_add_item(edit_menu, "Cut",    menu_cut);
    menu_add_item(edit_menu, "Paste",  menu_paste);
    menu_add_separator(edit_menu);
    menu_add_item(edit_menu, "Delete", menu_delete);

    menu *view_menu = window_add_menu(s->win, "View");
    menu_add_item(view_menu, "By Name", menu_sort_name);
    menu_add_item(view_menu, "By Date", menu_sort_date);
    menu_add_item(view_menu, "By Size", menu_sort_size);

    s->dir_cluster    = 0;
    s->sort           = SORT_NAME;
    s->drag_idx       = -1;
    s->last_click_idx = -1;
    s->mode           = MODE_NORMAL;
    strcpy(s->path, "/");

    ff_reload(s);
}

static void ff_destroy(void *state) {
    ff_inst_t *s = (ff_inst_t *)state;
    if (s->win) { wm_unregister(s->win); s->win = NULL; }
}

app_descriptor finder_app = {
    .name       = "Finder",
    .state_size = sizeof(ff_inst_t),
    .init       = ff_init,
    .on_frame   = ff_on_frame,
    .destroy    = ff_destroy,
};
