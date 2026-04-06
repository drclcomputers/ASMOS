#include "os/api.h"

#define FF_W        240
#define FF_H        160
#define FF_X        30
#define FF_Y        20

#define GRID_X      4
#define GRID_Y      4
#define CELL_W      56
#define CELL_H      32
#define COLS        4
#define ICON_W      16
#define ICON_H      14
#define LABEL_MAX   10
#define CHAR_W      5
#define LINE_H      8

#define MAX_ENTRIES 64

#define MODE_NORMAL  0
#define MODE_CONFIRM 1

typedef struct {
    dir_entry_t entry;
    char        display[LABEL_MAX + 1];
    bool        is_dotdot;
} ff_entry_t;

typedef struct {
    window     *win;

    ff_entry_t  entries[MAX_ENTRIES];
    int         entry_count;

    int         selected;
    int         scroll;
    int         visible_rows;

    char        clipboard[256];
    bool        clipboard_valid;

    char        status[64];

    int         mode;
    char        confirm_msg[80];
    void      (*confirm_yes)(void *state);
} ff_state_t;

app_descriptor finder_app;

static ff_state_t *ff_get_state(void) {
    app_instance_t *inst = os_find_instance(&finder_app);
    return inst ? (ff_state_t *)inst->state : NULL;
}

static void ff_reload(ff_state_t *s);
static void ff_draw(window *win, void *ud);

static bool ff_close_cb(window *w) {
    (void)w;
    os_quit_app_by_desc(&finder_app);
    return true;
}

static void entry_display_name(const dir_entry_t *e, char *out) {
    int j = 0;
    for (int k = 0; k < 8 && e->name[k] != ' '; k++) out[j++] = e->name[k];
    if (!(e->attr & ATTR_DIRECTORY) && e->ext[0] != ' ') {
        out[j++] = '.';
        for (int k = 0; k < 3 && e->ext[k] != ' '; k++) out[j++] = e->ext[k];
    }
    out[j] = '\0';
}

static void truncate_label(const char *src, char *out) {
    int i = 0;
    while (src[i] && i < LABEL_MAX) { out[i] = src[i]; i++; }
    if (src[i]) { out[LABEL_MAX - 2] = '.'; out[LABEL_MAX - 1] = '.'; }
    out[i < LABEL_MAX ? i : LABEL_MAX] = '\0';
}

static void ff_reload(ff_state_t *s) {
    dir_entry_t raw[MAX_ENTRIES];
    int count = 0;
    fat16_list_dir(dir_context.current_cluster, raw, MAX_ENTRIES, &count);

    s->entry_count = 0;
    s->selected    = -1;

    if (dir_context.current_cluster != 0) {
        ff_entry_t *up = &s->entries[s->entry_count++];
        memset(up, 0, sizeof(ff_entry_t));
        up->is_dotdot = true;
        strcpy(up->display, "..");
    }

    for (int i = 0; i < count && s->entry_count < MAX_ENTRIES; i++) {
        if (raw[i].name[0] == '.') continue;
        ff_entry_t *fe = &s->entries[s->entry_count++];
        fe->entry      = raw[i];
        fe->is_dotdot  = false;
        char full[13];
        entry_display_name(&raw[i], full);
        truncate_label(full, fe->display);
    }

    s->scroll  = 0;
    s->status[0] = '\0';
}

static void ff_full_name(ff_state_t *s, int idx, char *out, int max) {
    if (idx < 0 || idx >= s->entry_count) { out[0] = '\0'; return; }
    ff_entry_t *fe = &s->entries[idx];
    if (fe->is_dotdot) { strncpy(out, "..", max - 1); out[max-1] = '\0'; return; }
    char full[13];
    entry_display_name(&fe->entry, full);
    strncpy(out, full, max - 1);
    out[max - 1] = '\0';
}

static void draw_file_icon(int ax, int ay, bool is_dir, bool selected) {
    uint8_t bg = selected ? LIGHT_BLUE : WHITE;
    fill_rect(ax, ay, ICON_W, ICON_H, bg);
    draw_rect(ax, ay, ICON_W, ICON_H, BLACK);

    if (is_dir) {
        fill_rect(ax + 1, ay + 1, 6, 3, YELLOW);
        fill_rect(ax + 1, ay + 4, ICON_W - 2, ICON_H - 5, YELLOW);
        draw_rect(ax + 1, ay + 4, ICON_W - 2, ICON_H - 5, DARK_GRAY);
    } else {
        fill_rect(ax + 3, ay + 1, 7, 10, selected ? WHITE : LIGHT_GRAY);
        draw_rect(ax + 3, ay + 1, 7, 10, DARK_GRAY);
        draw_line(ax + 8, ay + 1, ax + 10, ay + 3, DARK_GRAY);
        draw_line(ax + 8, ay + 1, ax + 8,  ay + 3, DARK_GRAY);
        draw_line(ax + 8, ay + 3, ax + 10, ay + 3, DARK_GRAY);
        draw_line(ax + 4, ay + 5, ax + 9,  ay + 5, DARK_GRAY);
        draw_line(ax + 4, ay + 7, ax + 9,  ay + 7, DARK_GRAY);
    }
}

static int ff_content_top(ff_state_t *s) {
    return s->win->y + MENUBAR_H + 17 + LINE_H + 4;
}

static void ff_draw(window *win, void *ud) {
    ff_state_t *s = (ff_state_t *)ud;
    if (!win) return;

    int wx       = win->x + 1;
    int wy       = win->y + MENUBAR_H + 17;
    int ww       = win->w - 2;
    int wh       = win->h - 17;
    int grid_top = wy + LINE_H + 4;
    int grid_h   = wh - LINE_H - 4 - LINE_H - 2;

    char cwd[256];
    fat16_pwd(cwd, sizeof(cwd));
    draw_string(wx + 2, wy + 2, cwd, DARK_GRAY, 2);
    draw_line(wx, wy + LINE_H + 2, wx + ww - 1, wy + LINE_H + 2, DARK_GRAY);

    s->visible_rows = grid_h / CELL_H;
    if (s->visible_rows < 1) s->visible_rows = 1;

    for (int i = 0; i < s->visible_rows * COLS; i++) {
        int logical = s->scroll * COLS + i;
        if (logical >= s->entry_count) break;

        ff_entry_t *fe  = &s->entries[logical];
        bool selected   = (logical == s->selected);
        int col = i % COLS;
        int row = i / COLS;
        int cx  = wx + GRID_X + col * CELL_W;
        int cy  = grid_top + row * CELL_H;

        if (selected) fill_rect(cx, cy, CELL_W - 2, CELL_H - 2, LIGHT_BLUE);

        bool is_dir = fe->is_dotdot || (fe->entry.attr & ATTR_DIRECTORY);
        int ix = cx + (CELL_W - 2) / 2 - ICON_W / 2;
        int iy = cy + 1;
        draw_file_icon(ix, iy, is_dir, selected);

        int lw = (int)strlen(fe->display) * CHAR_W;
        int lx = cx + (CELL_W - 2) / 2 - lw / 2;
        draw_string(lx, iy + ICON_H + 2, fe->display, selected ? WHITE : BLACK, 2);
    }

    int sb_x = wx + ww - 6;
    int sb_y = grid_top;
    int sb_h = grid_h;
    fill_rect(sb_x, sb_y, 6, sb_h, LIGHT_GRAY);
    draw_rect(sb_x, sb_y, 6, sb_h, DARK_GRAY);
    int total_rows = (s->entry_count + COLS - 1) / COLS;
    if (total_rows > s->visible_rows && total_rows > 0) {
        int thumb_h = (s->visible_rows * sb_h) / total_rows;
        if (thumb_h < 4) thumb_h = 4;
        int max_scroll = total_rows - s->visible_rows;
        int thumb_y    = sb_y + (s->scroll * (sb_h - thumb_h)) / max_scroll;
        fill_rect(sb_x + 1, thumb_y, 4, thumb_h, DARK_GRAY);
    }

    int stat_y = wy + wh - LINE_H - 1;
    draw_line(wx, stat_y - 2, wx + ww - 1, stat_y - 2, DARK_GRAY);
    if (s->status[0]) {
        draw_string(wx + 2, stat_y, s->status, DARK_GRAY, 2);
    } else {
        char buf[32];
        sprintf(buf, "%d items", s->entry_count);
        draw_string(wx + 2, stat_y, buf, DARK_GRAY, 2);
    }

    if (s->mode == MODE_CONFIRM) {
        int bx = wx + 4;
        int by = grid_top + 4;
        int bw = ww - 8;
        int bh = 40;
        fill_rect(bx, by, bw, bh, LIGHT_GRAY);
        draw_rect(bx, by, bw, bh, BLACK);
        draw_string(bx + 4, by + 4, s->confirm_msg, BLACK, 2);
        fill_rect(bx + 4,  by + bh - 14, 30, 10, RED);
        draw_rect(bx + 4,  by + bh - 14, 30, 10, BLACK);
        draw_string(bx + 9,  by + bh - 12, "Yes", WHITE, 2);
        fill_rect(bx + 40, by + bh - 14, 30, 10, LIGHT_GRAY);
        draw_rect(bx + 40, by + bh - 14, 30, 10, BLACK);
        draw_string(bx + 45, by + bh - 12, "No",  BLACK, 2);
    }
}

static int ff_hit_entry(ff_state_t *s, int mx, int my) {
    int wx       = s->win->x + 1;
    int wy       = s->win->y + MENUBAR_H + 17;
    int wh       = s->win->h - 17;
    int grid_top = wy + LINE_H + 4;
    int grid_h   = wh - LINE_H - 4 - LINE_H - 2;
    (void)grid_h;

    for (int i = 0; i < s->visible_rows * COLS; i++) {
        int logical = s->scroll * COLS + i;
        if (logical >= s->entry_count) break;
        int col = i % COLS;
        int row = i / COLS;
        int cx  = wx + GRID_X + col * CELL_W;
        int cy  = grid_top + row * CELL_H;
        if (mx >= cx && mx < cx + CELL_W - 2 && my >= cy && my < cy + CELL_H - 2)
            return logical;
    }
    return -1;
}

static void do_delete(void *ud) {
    ff_state_t *s = (ff_state_t *)ud;
    if (s->selected < 0) return;
    char name[14];
    ff_full_name(s, s->selected, name, sizeof(name));
    dir_entry_t e;
    if (!fat16_find(name, &e)) { strcpy(s->status, "Not found"); ff_reload(s); return; }
    bool ok = (e.attr & ATTR_DIRECTORY) ? fat16_rm_rf(name) : fat16_delete(name);
    strcpy(s->status, ok ? "Deleted" : "Delete failed");
    s->selected = -1;
    s->mode = MODE_NORMAL;
    ff_reload(s);
}

static void cancel_confirm(void *ud) {
    ff_state_t *s = (ff_state_t *)ud;
    s->mode = MODE_NORMAL;
    strcpy(s->status, "Cancelled");
}

static void menu_new_file(void) {
    ff_state_t *s = ff_get_state(); if (!s) return;
    fat16_file_t f;
    if (!fat16_create("NEWFILE.TXT", &f)) {
        strcpy(s->status, "Error: already exists");
    } else {
        fat16_close(&f);
        strcpy(s->status, "Created NEWFILE.TXT");
    }
    ff_reload(s);
}

static void menu_new_folder(void) {
    ff_state_t *s = ff_get_state(); if (!s) return;
    if (!fat16_mkdir("NEWDIR")) {
        strcpy(s->status, "Error: already exists");
    } else {
        strcpy(s->status, "Created NEWDIR");
    }
    ff_reload(s);
}

static void menu_copy(void) {
    ff_state_t *s = ff_get_state(); if (!s) return;
    if (s->selected < 0 || s->entries[s->selected].is_dotdot) {
        strcpy(s->status, "Nothing selected"); return;
    }
    ff_full_name(s, s->selected, s->clipboard, sizeof(s->clipboard));
    s->clipboard_valid = true;
    strcpy(s->status, "Copied");
}

static void menu_paste(void) {
    ff_state_t *s = ff_get_state(); if (!s) return;
    if (!s->clipboard_valid) { strcpy(s->status, "Clipboard empty"); return; }

    dir_entry_t se;
    if (!fat16_find(s->clipboard, &se)) { strcpy(s->status, "Source gone"); return; }

    char dst[14];
    strncpy(dst, s->clipboard, 13); dst[13] = '\0';
    dir_entry_t de;
    if (fat16_find(dst, &de)) { strcpy(s->status, "Destination exists"); return; }

    bool ok = (se.attr & ATTR_DIRECTORY)
              ? fat16_copy_dir(s->clipboard, dst)
              : fat16_copy_file(s->clipboard, dst);
    strcpy(s->status, ok ? "Pasted" : "Paste failed");
    ff_reload(s);
}

static void menu_delete(void) {
    ff_state_t *s = ff_get_state(); if (!s) return;
    if (s->selected < 0 || s->entries[s->selected].is_dotdot) {
        strcpy(s->status, "Nothing selected"); return;
    }
    char name[14];
    ff_full_name(s, s->selected, name, sizeof(name));
    sprintf(s->confirm_msg, "Delete %s?", name);
    s->confirm_yes = do_delete;
    s->mode = MODE_CONFIRM;
}

static void menu_close(void) {
    os_quit_app_by_desc(&finder_app);
}

static void ff_on_frame(void *state) {
    ff_state_t *s = (ff_state_t *)state;
    if (!s->win || !s->win->visible) return;

    int wx       = s->win->x + 1;
    int wy       = s->win->y + MENUBAR_H + 17;
    int ww       = s->win->w - 2;
    int wh       = s->win->h - 17;
    int grid_top = wy + LINE_H + 4;
    int grid_h   = wh - LINE_H - 4 - LINE_H - 2;
    int sb_x     = wx + ww - 6;
    int sb_y     = grid_top;
    int sb_h     = grid_h;

    if (s->mode == MODE_CONFIRM) {
        if (mouse.left_clicked) {
            int bx = wx + 4;
            int by = grid_top + 4;
            int bh = 40;
            int yes_x = bx + 4,  yes_y = by + bh - 14;
            int no_x  = bx + 40, no_y  = yes_y;
            if (mouse.x >= yes_x && mouse.x < yes_x + 30 &&
                mouse.y >= yes_y && mouse.y < yes_y + 10) {
                if (s->confirm_yes) s->confirm_yes(s);
            } else if (mouse.x >= no_x && mouse.x < no_x + 30 &&
                       mouse.y >= no_y && mouse.y < no_y + 10) {
                cancel_confirm(s);
            }
        }
        ff_draw(s->win, s);
        return;
    }

    if (mouse.left_clicked || mouse.left) {
        if (mouse.x >= sb_x && mouse.x < sb_x + 6 &&
            mouse.y >= sb_y && mouse.y < sb_y + sb_h) {
            int total_rows = (s->entry_count + COLS - 1) / COLS;
            if (total_rows > s->visible_rows) {
                int new_scroll = ((mouse.y - sb_y) * (total_rows - s->visible_rows)) / sb_h;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > total_rows - s->visible_rows)
                    new_scroll = total_rows - s->visible_rows;
                s->scroll = new_scroll;
            }
        }
    }

    static uint32_t last_click_tick = 0;
    static int      last_click_idx  = -1;

    if (mouse.left_clicked) {
        int hit = ff_hit_entry(s, mouse.x, mouse.y);
        if (hit >= 0) {
            uint32_t now = pit_ticks_func();
            if (hit == last_click_idx && (now - last_click_tick) <= 60) {
                ff_entry_t *fe = &s->entries[hit];
                if (fe->is_dotdot) {
                    fat16_chdir("..");
                    ff_reload(s);
                } else if (fe->entry.attr & ATTR_DIRECTORY) {
                    char name[14];
                    ff_full_name(s, hit, name, sizeof(name));
                    fat16_chdir(name);
                    ff_reload(s);
                }
                last_click_idx  = -1;
                last_click_tick = 0;
            } else {
                s->selected     = hit;
                last_click_idx  = hit;
                last_click_tick = pit_ticks_func();
            }
        } else {
            s->selected    = -1;
            last_click_idx = -1;
        }
    }

    ff_draw(s->win, s);
}

static void ff_init(void *state) {
    ff_state_t *s = (ff_state_t *)state;

    const window_spec_t spec = {
        .x             = FF_X,
        .y             = FF_Y,
        .w             = FF_W,
        .h             = FF_H,
        .resizable     = true,
        .title         = "Finder",
        .title_color   = WHITE,
        .bar_color     = LIGHT_BLUE,
        .content_color = WHITE,
        .visible       = true,
        .on_close      = ff_close_cb,
    };
    s->win = wm_register(&spec);
    if (!s->win) return;

    s->win->on_draw          = ff_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "New File",   menu_new_file);
    menu_add_item(file_menu, "New Folder", menu_new_folder);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close",      menu_close);

    menu *edit_menu = window_add_menu(s->win, "Edit");
    menu_add_item(edit_menu, "Copy",   menu_copy);
    menu_add_item(edit_menu, "Paste",  menu_paste);
    menu_add_item(edit_menu, "Delete", menu_delete);

    s->selected        = -1;
    s->clipboard_valid = false;
    s->mode            = MODE_NORMAL;

    ff_reload(s);
}

static void ff_destroy(void *state) {
    ff_state_t *s = (ff_state_t *)state;
    if (s->win) { wm_unregister(s->win); s->win = NULL; }
}

app_descriptor finder_app = {
    .name       = "FileFinder",
    .state_size = sizeof(ff_state_t),
    .init       = ff_init,
    .on_frame   = ff_on_frame,
    .destroy    = ff_destroy,
};
