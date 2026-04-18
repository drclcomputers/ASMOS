#include "os/api.h"
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

#define SCROLLBAR_W 4

#define MAX_ENTRIES 128
#define SORT_NAME 0
#define SORT_DATE 1
#define SORT_SIZE 2
#define MODE_NORMAL 0
#define MODE_RENAME 1
#define MODE_CONFIRM 2
#define MODE_NEWNAME 3
#define MODE_INFO 4

typedef struct {
    dir_entry_t entry;
    char name[13];
    char label[LABEL_CHARS + 1];
    bool is_dotdot;
    int icon_x, icon_y;
    bool selected;
    bool dragging;
    int drag_off_x, drag_off_y;
    bool pinned;
} ff_item_t;

typedef struct {
    window *win;
    uint16_t dir_cluster;
    char path[256];

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
} ff_inst_t;

typedef struct {
    char path[256];
    char name[13];
    bool is_dir;
    bool is_cut;
    bool valid;
} ff_clip_t;

static ff_clip_t g_clip = {.valid = false};

app_descriptor filef_app;

static int s_cascade = 0;

static void ff_reload(ff_inst_t *s);
static void ff_draw_window(window *win, void *ud);
static void ff_open_dir(uint16_t cluster, const char *path);

void ff_open_dir_pub(uint16_t cluster, const char *path) {
    ff_open_dir(cluster, path);
}

static void menu_new_file(void);
static void menu_new_folder(void);
static void menu_rename(void);
static void menu_reload(void);
static void menu_close(void);
static void menu_copy(void);
static void menu_cut(void);
static void menu_paste(void);
static void menu_delete(void);
static void menu_get_info(void);
static void menu_sort_name(void);
static void menu_sort_date(void);
static void menu_sort_size(void);
static void cancel_confirm(void *ud);
static int ff_hit(ff_inst_t *s, int mx, int my);
static bool in_content(ff_inst_t *s, int mx, int my);

static void maybe_notify_desktop(ff_inst_t *s) {
    if (strcmp(s->path, desktop_fs_path()) == 0)
        desktop_fs_set_dirty();
}

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
    if (!fw)
        return NULL;
    return inst_of(fw);
}

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

static bool fat_char_ok(char c) {
    if (c >= 'A' && c <= 'Z')
        return true;
    if (c >= 'a' && c <= 'z')
        return true;
    if (c >= '0' && c <= '9')
        return true;
    const char *ok = "!#$%&'()-@^_`{}~";
    for (int i = 0; ok[i]; i++)
        if (c == ok[i])
            return true;
    return false;
}

static bool validate_fat_name(const char *name_buf, bool is_dir,
                              const char **err_msg) {
    if (!name_buf || name_buf[0] == '\0') {
        *err_msg = "Name cannot be empty.";
        return false;
    }
    const char *dot = strchr(name_buf, '.');
    int base_len, ext_len;
    if (dot) {
        base_len = (int)(dot - name_buf);
        ext_len = (int)strlen(dot + 1);
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
        ext_len = 0;
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
    if (a->is_dotdot)
        return -1;
    if (b->is_dotdot)
        return 1;
    bool ad = (a->entry.attr & ATTR_DIRECTORY) != 0;
    bool bd = (b->entry.attr & ATTR_DIRECTORY) != 0;
    if (ad != bd)
        return ad ? -1 : 1;
    if (sort == SORT_SIZE)
        return (int)a->entry.file_size - (int)b->entry.file_size;
    if (sort == SORT_DATE) {
        int dc = (int)b->entry.modify_date - (int)a->entry.modify_date;
        if (dc != 0)
            return dc;
        return (int)b->entry.modify_time - (int)a->entry.modify_time;
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

static int content_clip_bottom(ff_inst_t *s) {
    int wh = s->win->h - 16;
    return (s->win->y + MENUBAR_H + 16) + wh - STATUS_H - CONTENT_BOT;
}

static void ff_layout(ff_inst_t *s) {
    int ww = s->win->w - 2 - SCROLLBAR_W - 2;
    int cols = (ww - 4) / ICON_CELL_W;
    if (cols < 1)
        cols = 1;
    int col = 0, row = 0;
    int max_row = -1;

    for (int i = 0; i < s->item_count; i++) {
        ff_item_t *it = &s->items[i];
        if (it->pinned)
            continue;
        if (it->dragging)
            continue;
        it->icon_x = 4 + col * ICON_CELL_W;
        it->icon_y = CONTENT_TOP + row * ICON_CELL_H;
        if (row > max_row)
            max_row = row;
        col++;
        if (col >= cols) {
            col = 0;
            row++;
        }
    }

    s->content_h_total =
        CONTENT_TOP + (max_row >= 0 ? (max_row + 1) * ICON_CELL_H : 0);
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
        if (raw[i].name[0] == '.')
            continue;
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
    s->scroll_y = 0;
}

static bool item_is_app(const ff_item_t *it) {
    if (it->is_dotdot)
        return false;
    if (it->entry.attr & ATTR_DIRECTORY)
        return false;
    return (it->entry.ext[0] == 'A' && it->entry.ext[1] == 'P' &&
            it->entry.ext[2] == 'P');
}

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

    if (it->is_dotdot) {
        draw_dotdot_icon(ax, ay, it->selected);
    } else if (it->entry.attr & ATTR_DIRECTORY) {
        draw_folder_icon(ax, ay, it->selected);
    } else if (item_is_app(it)) {
        draw_app_icon(ax, ay, it->selected);
    } else {
        char ext[4] = {0};
        ext[0] = it->entry.ext[0];
        ext[1] = it->entry.ext[1];
        ext[2] = it->entry.ext[2];
        ext[3] = '\0';
        for (int j = 2; j >= 0; j--) {
            if (ext[j] == ' ')
                ext[j] = '\0';
            else
                break;
        }
        if (strcmp(ext, "PIC") == 0 || strcmp(ext, "pic") == 0) {
            draw_pic_icon(ax, ay, it->selected);
        } else if (strcmp(ext, "TXT") == 0 || strcmp(ext, "txt") == 0) {
            draw_txt_icon(ax, ay, it->selected);
        } else if (strcmp(ext, "CFG") == 0 || strcmp(ext, "cfg") == 0) {
            draw_cfg_icon(ax, ay, it->selected);
        } else {
            draw_unknown_icon(ax, ay, it->selected);
        }
    }

    int lw = (int)strlen(it->label) * CHAR_W;
    int lx = ax + ICON_SZ_W / 2 - lw / 2;
    int ly = ay + ICON_SZ_H + 1;

    if (it->selected) {
        fill_rect(lx - 1, ly - 1, lw + 2, CHAR_H + 2, BLACK);
        draw_string(lx, ly, it->label, WHITE, 2);
    } else {
        draw_string(lx + 1, ly + 1, it->label, WHITE, 2);
        draw_string(lx, ly, it->label, BLACK, 2);
    }

    if (s->mode == MODE_RENAME && i == s->rename_idx) {
        fill_rect(ax - 1, ay + ICON_SZ_H + 1 - 1, ICON_CELL_W, CHAR_H + 2,
                  WHITE);
        draw_rect(ax - 1, ay + ICON_SZ_H + 1 - 1, ICON_CELL_W, CHAR_H + 2,
                  BLACK);
        draw_string(ax, ay + ICON_SZ_H + 1, s->rename_buf, BLACK, 2);
        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 50) % 2 == 0) {
            int cx = ax + s->rename_len * CHAR_W;
            draw_string(cx, ay + ICON_SZ_H + 1, "|", BLACK, 2);
        }
    }
}

static void ff_draw_info_overlay(ff_inst_t *s, int wx, int wy, int ww, int wh) {
    int pw = ww - 16;
    int ph = wh - 20;
    int px = wx + 8;
    int py = wy + 10;

    fill_rect(px + 3, py + 3, pw, ph, BLACK);
    fill_rect(px, py, pw, ph, LIGHT_GRAY);
    draw_rect(px, py, pw, ph, BLACK);
    fill_rect(px, py, pw, 12, DARK_GRAY);
    draw_string(px + 4, py + 3, "Get Info", WHITE, 2);

    fill_rect(px + pw - 14, py + 2, 10, 8, RED);
    draw_rect(px + pw - 14, py + 2, 10, 8, BLACK);
    draw_string(px + pw - 12, py + 3, "X", WHITE, 2);

    draw_line(px, py + 13, px + pw - 1, py + 13, BLACK);

    int row_y = py + 17;
    int lx = px + 4;
    int vx = px + 50;
    int line_h = 10;

    draw_string(lx, row_y, "Name:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_name, BLACK, 2);
    row_y += line_h;

    draw_string(lx, row_y, "Path:", DARK_GRAY, 2);
    int max_path_chars = (pw - 54) / CHAR_W;
    int plen = (int)strlen(s->info_path);
    if (plen <= max_path_chars) {
        draw_string(vx, row_y, s->info_path, BLACK, 2);
    } else {
        char trunc[64];
        strcpy(trunc, "...");
        int start = plen - (max_path_chars - 3);
        if (start < 0)
            start = 0;
        strcat(trunc, s->info_path + start);
        draw_string(vx, row_y, trunc, BLACK, 2);
    }
    row_y += line_h;

    draw_string(lx, row_y, "Type:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_type, BLACK, 2);
    row_y += line_h;

    draw_string(lx, row_y, "Size:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_size, BLACK, 2);
    row_y += line_h;

    draw_string(lx, row_y, "Date:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_date, BLACK, 2);
    row_y += line_h;

    draw_string(lx, row_y, "Time:", DARK_GRAY, 2);
    draw_string(vx, row_y, s->info_time, BLACK, 2);

    int btn_x = px + pw / 2 - 20;
    int btn_y = py + ph - 14;
    fill_rect(btn_x, btn_y, 40, 10, LIGHT_BLUE);
    draw_rect(btn_x, btn_y, 40, 10, BLACK);
    draw_string(btn_x + 13, btn_y + 2, "OK", WHITE, 2);
}

static void ff_populate_info(ff_inst_t *s, int sel) {
    ff_item_t *it = &s->items[sel];
    strncpy(s->info_name, it->name, 12);
    s->info_name[12] = '\0';

    if (s->path[0] == '/' && s->path[1] == '\0')
        sprintf(s->info_path, "/%s", it->name);
    else
        sprintf(s->info_path, "%s/%s", s->path, it->name);

    if (it->is_dotdot) {
        strcpy(s->info_type, "Parent Dir");
        strcpy(s->info_size, "-");
    } else if (it->entry.attr & ATTR_DIRECTORY) {
        strcpy(s->info_type, "Folder");
        strcpy(s->info_size, "-");
    } else if (item_is_app(it)) {
        strcpy(s->info_type, "Application");
        sprintf(s->info_size, "%u B", it->entry.file_size);
    } else {
        const char *ext = "";
        const char *dot = strchr(it->name, '.');
        if (dot)
            ext = dot + 1;
        if (strcmp(ext, "TXT") == 0 || strcmp(ext, "txt") == 0)
            strcpy(s->info_type, "Text File");
        else if (strlen(ext) == 0)
            strcpy(s->info_type, "File");
        else {
            strcpy(s->info_type, ext);
            strcat(s->info_type, " File");
        }
        sprintf(s->info_size, "%u B", it->entry.file_size);
    }

    uint16_t fat_date = it->entry.modify_date;
    uint16_t fat_time = it->entry.modify_time;
    uint16_t year = ((fat_date >> 9) & 0x7F) + 1980;
    uint8_t month = (fat_date >> 5) & 0x0F;
    uint8_t day = fat_date & 0x1F;
    uint8_t hour = (fat_time >> 11) & 0x1F;
    uint8_t min = (fat_time >> 5) & 0x3F;
    uint8_t sec = (fat_time & 0x1F) * 2;

    if (fat_date == 0) {
        strcpy(s->info_date, "Unknown");
        strcpy(s->info_time, "Unknown");
    } else {
        sprintf(s->info_date, "%04d/%02d/%02d", year, month, day);
        sprintf(s->info_time, "%02d:%02d:%02d", hour, min, sec);
    }
}

static void ff_draw_scrollbar(ff_inst_t *s, int wx, int wy, int ww, int wh) {
    int view_h = wh - CONTENT_TOP - CONTENT_BOT - STATUS_H - 1;
    if (view_h < 1)
        view_h = 1;

    int sb_x = wx + ww - SCROLLBAR_W - 1;
    int sb_y = wy + CONTENT_TOP;
    int sb_h = view_h;

    fill_rect(sb_x, sb_y, SCROLLBAR_W, sb_h, LIGHT_GRAY);

    if (s->content_h_total > view_h) {
        int max_scroll = s->content_h_total - view_h;
        if (max_scroll < 1)
            max_scroll = 1;

        int thumb_h = (view_h * sb_h) / s->content_h_total;
        if (thumb_h < 6)
            thumb_h = 6;
        if (thumb_h > sb_h)
            thumb_h = sb_h;

        int thumb_range = sb_h - thumb_h;
        int thumb_y = sb_y;
        if (thumb_range > 0)
            thumb_y += (s->scroll_y * thumb_range) / max_scroll;

        fill_rect(sb_x + 1, thumb_y + 1, SCROLLBAR_W - 2, thumb_h - 2,
                  DARK_GRAY);
    }
}

static void ff_draw_window(window *win, void *ud) {
    ff_inst_t *s = (ff_inst_t *)ud;
    if (!s)
        return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;
    int content_y = wy + CONTENT_TOP;
    int content_h = wh - CONTENT_TOP - CONTENT_BOT - STATUS_H;

    fill_rect(wx, wy, ww, wh, WHITE);

    draw_line(wx, wy + CONTENT_TOP - 1, wx + ww - 1, wy + CONTENT_TOP - 1,
              LIGHT_GRAY);

    int stat_y = wy + wh - STATUS_H - CONTENT_BOT;
    fill_rect(wx, stat_y, ww, STATUS_H + CONTENT_BOT, LIGHT_GRAY);
    draw_line(wx, stat_y, wx + ww - 1, stat_y, LIGHT_GRAY);

    char stat[64];
    if (s->status[0]) {
        strncpy(stat, s->status, 63);
        stat[63] = '\0';
    } else {
        sprintf(
            stat, "%d item%s", s->item_count - (s->dir_cluster != 0 ? 1 : 0),
            (s->item_count - (s->dir_cluster != 0 ? 1 : 0)) == 1 ? "" : "s");
    }
    draw_string(wx + 3, stat_y + 1, stat, DARK_GRAY, 2);

    int clip_bottom = stat_y;

    for (int i = 0; i < s->item_count; i++) {
        if (s->items[i].dragging)
            continue;
        ff_draw_item(s, i, wx, content_y - s->scroll_y, clip_bottom);
    }

    if (s->drag_idx >= 0) {
        ff_item_t *it = &s->items[s->drag_idx];
        int ax = mouse.x - it->drag_off_x;
        int ay = mouse.y - it->drag_off_y;
        int save_x = it->icon_x, save_y = it->icon_y;
        it->icon_x = ax - wx;
        it->icon_y = ay - content_y + s->scroll_y;
        ff_draw_item(s, s->drag_idx, wx, content_y - s->scroll_y, clip_bottom);
        it->icon_x = save_x;
        it->icon_y = save_y;
    }

    ff_draw_scrollbar(s, wx, wy, ww, wh);

    if (s->mode == MODE_CONFIRM) {
        int bx = wx + 4, by = wy + content_h / 2 - 16;
        int bw = ww - 8;
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

    if (s->mode == MODE_INFO) {
        ff_draw_info_overlay(s, wx, wy, ww, wh);
    }
}

static int content_view_h(ff_inst_t *s) {
    int wh = s->win->h - 16;
    return wh - CONTENT_TOP - CONTENT_BOT - STATUS_H - 1;
}

static int ff_hit(ff_inst_t *s, int mx, int my) {
    if (!in_content(s, mx, my))
        return -1;
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16 + CONTENT_TOP;
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

static bool in_content(ff_inst_t *s, int mx, int my) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16 + CONTENT_TOP;
    int ww = s->win->w - 2 - SCROLLBAR_W - 2;
    int wh = content_view_h(s);
    return (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh);
}

static void do_delete(void *ud) {
    ff_inst_t *s = (ff_inst_t *)ud;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) {
            sel = i;
            break;
        }
    if (sel < 0) {
        s->mode = MODE_NORMAL;
        return;
    }

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
    maybe_notify_desktop(s);
    ff_reload(s);
}

static void cancel_confirm(void *ud) {
    ff_inst_t *s = (ff_inst_t *)ud;
    s->mode = MODE_NORMAL;
}

static void menu_new_file(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    s->newname_buf[0] = '\0';
    s->newname_len = 0;
    s->newname_is_dir = false;
    s->mode = MODE_NEWNAME;
}

static void menu_new_folder(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    s->newname_buf[0] = '\0';
    s->newname_len = 0;
    s->newname_is_dir = true;
    s->mode = MODE_NEWNAME;
}

static void menu_rename(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
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

static void menu_copy(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
        return;
    }
    strncpy(g_clip.name, s->items[sel].name, 12);
    g_clip.name[12] = '\0';
    strncpy(g_clip.path, s->path, 255);
    g_clip.path[255] = '\0';
    g_clip.is_dir = (s->items[sel].entry.attr & ATTR_DIRECTORY) != 0;
    g_clip.is_cut = false;
    g_clip.valid = true;
    strcpy(s->status, "Copied.");
}

static void menu_cut(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
        return;
    }
    strncpy(g_clip.name, s->items[sel].name, 12);
    g_clip.name[12] = '\0';
    strncpy(g_clip.path, s->path, 255);
    g_clip.path[255] = '\0';
    g_clip.is_dir = (s->items[sel].entry.attr & ATTR_DIRECTORY) != 0;
    g_clip.is_cut = true;
    g_clip.valid = true;
    strcpy(s->status, "Cut.");
}

static void menu_paste(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    if (!g_clip.valid) {
        strcpy(s->status, "Clipboard empty.");
        return;
    }

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
        if (ok)
            g_clip.valid = false;
    } else {
        ok = g_clip.is_dir ? fat16_copy_dir(src_path, g_clip.name)
                           : fat16_copy_file(src_path, g_clip.name);
    }

    dir_context.current_cluster = saved;
    strcpy(s->status, ok ? "Pasted." : "Paste failed.");
    maybe_notify_desktop(s);
    ff_reload(s);
}

static void menu_delete(void) {
    ff_inst_t *s = active_finder();
    if (!s)
        return;
    int sel = -1;
    for (int i = 0; i < s->item_count; i++)
        if (s->items[i].selected && !s->items[i].is_dotdot) {
            sel = i;
            break;
        }
    if (sel < 0) {
        strcpy(s->status, "Nothing selected.");
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
    modal_show(MODAL_INFO, "About FileF",
               "FileF v1.1\nASMOS File Manager\nDrag, drop, copy & move files.",
               NULL, NULL);
}

static bool ff_close_cb(window *w) {
    ff_inst_t *s = inst_of(w);
    if (!s)
        return true;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (running_apps[i].running && running_apps[i].desc == &filef_app &&
            running_apps[i].state == s) {
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
        if (ok)
            fat16_close(&f);
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
    maybe_notify_desktop(s);
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
        modal_show(MODAL_ERROR, "Rename Failed", "Name already exists.", NULL,
                   NULL);
        return;
    }
    strcpy(s->status, "Renamed.");
    s->mode = MODE_NORMAL;
    maybe_notify_desktop(s);
    ff_reload(s);
}

static ff_inst_t *ff_find_target(int mx, int my, ff_inst_t *exclude) {
    for (int i = win_count - 1; i >= 0; i--) {
        window *w = win_stack[i];
        if (!w->visible)
            continue;
        ff_inst_t *t = inst_of(w);
        if (!t || t == exclude)
            continue;
        if (in_content(t, mx, my))
            return t;
    }
    return NULL;
}

static bool over_desktop(int mx, int my) {
    if (my < MENUBAR_H)
        return false;
    if (my >= SCREEN_HEIGHT - TASKBAR_H)
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

static bool is_ancestor_of(uint16_t src_cluster, uint16_t candidate_cluster) {
    if (src_cluster == candidate_cluster)
        return true;
    uint16_t cur = candidate_cluster;
    int depth = 0;
    while (cur != 0 && depth < 64) {
        if (cur == src_cluster)
            return true;
        dir_entry_t dotdot;
        char name83[12];
        fat16_make_83("..", name83);
        if (!fat16_find_in_dir(cur, name83, &dotdot))
            break;
        uint16_t parent = dotdot.cluster_lo;
        if (parent == cur)
            break;
        cur = parent;
        depth++;
    }
    return false;
}

static bool ff_drop_move(ff_inst_t *src, int drag_idx, ff_inst_t *dst) {
    ff_item_t *it = &src->items[drag_idx];
    if (it->is_dotdot)
        return false;
    if (src->dir_cluster == dst->dir_cluster)
        return false;

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
        maybe_notify_desktop(src);
        maybe_notify_desktop(dst);
        ff_reload(src);
        ff_reload(dst);
    } else {
        strcpy(src->status, "Move failed.");
    }
    return ok;
}

static bool ff_drop_to_desktop(ff_inst_t *src, int drag_idx) {
    ff_item_t *it = &src->items[drag_idx];
    if (it->is_dotdot)
        return false;
    if (src->dir_cluster == desktop_fs_cluster())
        return false;

    char src_path[270];
    if (src->path[0] == '/' && src->path[1] == '\0')
        sprintf(src_path, "/%s", it->name);
    else
        sprintf(src_path, "%s/%s", src->path, it->name);

    bool ok = desktop_accept_drop(src_path, it->name);
    if (ok) {
        strcpy(src->status, "Moved to Desktop.");
        ff_reload(src);
    } else {
        strcpy(src->status, "Drop to Desktop failed.");
    }
    return ok;
}

static void ff_open_dir(uint16_t cluster, const char *path) {
    app_instance_t *inst = os_launch_app(&filef_app);
    if (!inst)
        return;
    ff_inst_t *s = (ff_inst_t *)inst->state;
    s->dir_cluster = cluster;
    strncpy(s->path, path, 255);
    s->path[255] = '\0';
    s->win->title = s->path;
    ff_reload(s);
}

static void ff_scroll(ff_inst_t *s, int delta) {
    int view_h = content_view_h(s);
    int max_scroll = s->content_h_total - view_h;
    if (max_scroll < 0)
        max_scroll = 0;
    s->scroll_y += delta;
    if (s->scroll_y < 0)
        s->scroll_y = 0;
    if (s->scroll_y > max_scroll)
        s->scroll_y = max_scroll;
}

static void ff_on_frame(void *state) {
    ff_inst_t *s = (ff_inst_t *)state;
    if (!s->win || !s->win->visible)
        return;

    if (s->win->w != s->last_win_w || s->win->h != s->last_win_h) {
        s->last_win_w = s->win->w;
        s->last_win_h = s->win->h;
        ff_layout(s);

        int view_h = content_view_h(s);
        int max_scroll = s->content_h_total - view_h;
        if (max_scroll < 0)
            max_scroll = 0;
        if (s->scroll_y > max_scroll)
            s->scroll_y = max_scroll;
    }

    if (strcmp(s->path, desktop_fs_path()) == 0 && desktop_fs_is_dirty())
        ff_reload(s);

    int wx = s->win->x + 1;
    int wy_top = s->win->y + MENUBAR_H + 16 + CONTENT_TOP;
    int wh_cont = content_view_h(s);
    int conf_by = wy_top + wh_cont / 2 - 16;

    {
        int ww = s->win->w - 2;
        int wy = s->win->y + MENUBAR_H + 16;
        int sb_x = wx + ww - SCROLLBAR_W - 1;
        int sb_y = wy + CONTENT_TOP;
        int sb_h = wh_cont;

        if ((mouse.left_clicked || mouse.left) && mouse.x >= sb_x &&
            mouse.x < sb_x + SCROLLBAR_W + 1 && mouse.y >= sb_y &&
            mouse.y < sb_y + sb_h && s->content_h_total > wh_cont) {
            int max_scroll = s->content_h_total - wh_cont;
            int new_scroll = ((mouse.y - sb_y) * max_scroll) / sb_h;
            if (new_scroll < 0)
                new_scroll = 0;
            if (new_scroll > max_scroll)
                new_scroll = max_scroll;
            s->scroll_y = new_scroll;
        }
    }

    if (kb.key_pressed) {
        if (kb.last_scancode == F5)
            ff_scroll(s, -wh_cont);
        if (kb.last_scancode == F6)
            ff_scroll(s, wh_cont);
    }

    if (s->mode == MODE_INFO) {
        if (mouse.left_clicked) {
            int wx2 = s->win->x + 1;
            int wy2 = s->win->y + MENUBAR_H + 16;
            int ww2 = s->win->w - 2;
            int wh2 = s->win->h - 16;
            int pw = ww2 - 16;
            int ph = wh2 - 20;
            int px = wx2 + 8;
            int py = wy2 + 10;

            int cx = px + pw - 14;
            int cy = py + 2;
            if (mouse.x >= cx && mouse.x < cx + 10 && mouse.y >= cy &&
                mouse.y < cy + 8) {
                s->mode = MODE_NORMAL;
                ff_draw_window(s->win, s);
                return;
            }
            int btn_x = px + pw / 2 - 20;
            int btn_y = py + ph - 14;
            if (mouse.x >= btn_x && mouse.x < btn_x + 40 && mouse.y >= btn_y &&
                mouse.y < btn_y + 10) {
                s->mode = MODE_NORMAL;
                ff_draw_window(s->win, s);
                return;
            }
        }
        ff_draw_window(s->win, s);
        return;
    }

    if (s->mode == MODE_NEWNAME || s->mode == MODE_RENAME) {
        if (kb.key_pressed) {
            char *buf =
                (s->mode == MODE_NEWNAME) ? s->newname_buf : s->rename_buf;
            int *len =
                (s->mode == MODE_NEWNAME) ? &s->newname_len : &s->rename_len;
            int max = 12;
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
            } else if (kb.last_char >= 32 && kb.last_char < 127 && *len < max) {
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
        ff_draw_window(s->win, s);
        return;
    }

    if (s->mode == MODE_CONFIRM) {
        if (mouse.left_clicked) {
            int bx = wx + 4;
            int by = conf_by;
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

    if (s->drag_idx >= 0) {
        ff_item_t *it = &s->items[s->drag_idx];
        if (mouse.left) {
            int new_ix = mouse.x - it->drag_off_x - wx + s->scroll_y;
            int new_ix2 = mouse.x - it->drag_off_x - wx;
            int new_iy = mouse.y - it->drag_off_y - wy_top + s->scroll_y;
            it->icon_x = new_ix2;
            it->icon_y = new_iy;
            (void)new_ix;
        } else {
            if (!in_content(s, mouse.x, mouse.y)) {
                ff_inst_t *dst = ff_find_target(mouse.x, mouse.y, s);
                if (dst) {
                    ff_drop_move(s, s->drag_idx, dst);
                } else if (over_desktop(mouse.x, mouse.y)) {
                    ff_drop_to_desktop(s, s->drag_idx);
                } else {
                    it->dragging = false;
                    it->pinned = false;
                    ff_layout(s);
                }
            } else {
                it->dragging = false;
                it->pinned = true;
            }
            s->drag_idx = -1;
        }
        ff_draw_window(s->win, s);
        return;
    }

    if (mouse.left && !mouse.left_clicked && s->drag_idx < 0) {
        for (int i = 0; i < s->item_count; i++) {
            ff_item_t *it = &s->items[i];
            if (!it->selected || it->is_dotdot)
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

    if (mouse.left_clicked) {
        int hit = ff_hit(s, mouse.x, mouse.y);
        if (hit >= 0) {
            for (int i = 0; i < s->item_count; i++)
                s->items[i].selected = false;
            s->items[hit].selected = true;

            uint32_t now = pit_ticks_func();
            if (hit == s->last_click_idx && (now - s->last_click_tick) <= 60) {
                ff_item_t *it = &s->items[hit];
                if (it->is_dotdot) {
                    char parent[256];
                    char *slash = strrchr(s->path, '/');
                    if (slash && slash != s->path) {
                        int len = (int)(slash - s->path);
                        strncpy(parent, s->path, len);
                        parent[len] = '\0';
                    } else {
                        strcpy(parent, "/");
                    }
                    uint16_t saved = dir_context.current_cluster;
                    dir_context.current_cluster = s->dir_cluster;
                    fat16_chdir("..");
                    uint16_t parent_cluster = dir_context.current_cluster;
                    dir_context.current_cluster = saved;
                    ff_open_dir(parent_cluster, parent);
                } else if (item_is_app(it)) {
                    char app_name[9];
                    int ai = 0;
                    for (int k = 0; it->name[k] && it->name[k] != '.' && ai < 8;
                         k++)
                        app_name[ai++] = it->name[k];
                    app_name[ai] = '\0';
                    char proper[9];
                    for (int k = 0; app_name[k]; k++) {
                        char c = app_name[k];
                        if (k == 0 && c >= 'A' && c <= 'Z')
                            proper[k] = c;
                        else if (c >= 'A' && c <= 'Z')
                            proper[k] = c - 'A' + 'a';
                        else
                            proper[k] = c;
                    }
                    proper[ai] = '\0';
                    app_descriptor *desc = os_find_app(app_name);
                    if (!desc)
                        desc = os_find_app(proper);
                    proper[0] = app_name[0];
                    if (!desc) {
                        for (int k = 0; k < ai; k++) {
                            char c = app_name[k];
                            proper[k] =
                                (k == 0) ? (c >= 'a' && c <= 'z' ? c - 32 : c)
                                         : (c >= 'A' && c <= 'Z' ? c + 32 : c);
                        }
                        proper[ai] = '\0';
                        desc = os_find_app(proper);
                    }
                    if (desc) {
                        os_launch_app(desc);
                    } else {
                        char err_msg[256];
                        sprintf(err_msg, "Could not find application '%s'.",
                                app_name);
                        modal_show(MODAL_ERROR, "App Not Found", err_msg, NULL,
                                   NULL);
                    }
                } else if (it->entry.attr & ATTR_DIRECTORY) {
                    char child_path[270];
                    if (s->path[0] == '/' && s->path[1] == '\0')
                        sprintf(child_path, "/%s", it->name);
                    else
                        sprintf(child_path, "%s/%s", s->path, it->name);
                    ff_open_dir(it->entry.cluster_lo, child_path);
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

static void ff_init(void *state) {
    ff_inst_t *s = (ff_inst_t *)state;
    s->scroll_y = 0;
    s->last_win_w = FF_DEFAULT_W;
    s->last_win_h = FF_DEFAULT_H;

    int x = FF_DEFAULT_X + (s_cascade % 6) * FF_CASCADE;
    int y = FF_DEFAULT_Y + (s_cascade % 6) * FF_CASCADE;
    s_cascade++;

    const window_spec_t spec = {
        .x = x,
        .y = y,
        .w = FF_DEFAULT_W,
        .h = FF_DEFAULT_H,
        .min_w = 60,
        .min_h = 70,
        .resizable = true,
        .title = "Finder",
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

    menu *view_menu = window_add_menu(s->win, "View");
    menu_add_item(view_menu, "By Name", menu_sort_name);
    menu_add_item(view_menu, "By Date", menu_sort_date);
    menu_add_item(view_menu, "By Size", menu_sort_size);

    s->dir_cluster = desktop_fs_cluster();
    s->sort = SORT_NAME;
    s->drag_idx = -1;
    s->last_click_idx = -1;
    s->mode = MODE_NORMAL;
    strcpy(s->path, desktop_fs_path());
    s->win->title = s->path;

    ff_reload(s);
}

static void ff_destroy(void *state) {
    ff_inst_t *s = (ff_inst_t *)state;
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
