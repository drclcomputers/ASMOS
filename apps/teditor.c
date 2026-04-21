#include "os/api.h"

#define teditor_DEFAULT_X 30
#define teditor_DEFAULT_Y 20
#define teditor_DEFAULT_W 240
#define teditor_DEFAULT_H 170

#define CHAR_W 5
#define CHAR_H 6
#define LINE_H 8
#define MARGIN_X 4
#define MARGIN_Y 4
#define SCROLLBAR_W 6
#define SCROLLBAR_H 6
#define STATUS_H 8

#define TEXT_CAP 8192
#define MAX_LINES 512
#define MAX_COLS 120

#define FNAME_BUF_CAP 256

#define FNAME_MODE_NONE 0
#define FNAME_MODE_OPEN 1
#define FNAME_MODE_SAVEAS 2

typedef struct {
    window *win;

    char text[TEXT_CAP];
    int text_len;

    int cursor;

    int scroll_line;
    int visible_rows;

    int scroll_col;
    int max_line_len;

    int line_start[MAX_LINES];
    int line_count;

    bool dirty;

    char filepath[FNAME_BUF_CAP];
    char filename[FNAME_BUF_CAP];

    int fname_mode;
    char fname_buf[FNAME_BUF_CAP];
    int fname_len;

    char status[64];
    uint32_t status_timer;

} teditor_state_t;

app_descriptor teditor_app;

static void np_set_status(teditor_state_t *s, const char *msg) {
    strncpy(s->status, msg, 63);
    s->status[63] = '\0';
    s->status_timer = 300;
}

static void np_reindex(teditor_state_t *s) {
    s->line_count = 0;
    s->max_line_len = 0;
    s->line_start[s->line_count++] = 0;

    int cur_len = 0;
    for (int i = 0; i < s->text_len && s->line_count < MAX_LINES; i++) {
        if (s->text[i] == '\n') {
            if (cur_len > s->max_line_len)
                s->max_line_len = cur_len;
            cur_len = 0;
            s->line_start[s->line_count++] = i + 1;
        } else {
            cur_len++;
        }
    }
    if (cur_len > s->max_line_len)
        s->max_line_len = cur_len;
}

static int np_line_of(teditor_state_t *s, int pos) {
    int lo = 0, hi = s->line_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s->line_start[mid] <= pos)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static int np_col_of(teditor_state_t *s, int pos) {
    int ln = np_line_of(s, pos);
    return pos - s->line_start[ln];
}

static void np_scroll_to_cursor(teditor_state_t *s) {
    int ln = np_line_of(s, s->cursor);
    if (ln < s->scroll_line)
        s->scroll_line = ln;
    if (ln >= s->scroll_line + s->visible_rows)
        s->scroll_line = ln - s->visible_rows + 1;
    if (s->scroll_line < 0)
        s->scroll_line = 0;

    int col = np_col_of(s, s->cursor);
    if (col < s->scroll_col)
        s->scroll_col = col;
    if (s->scroll_col < 0)
        s->scroll_col = 0;
}

static void np_insert(teditor_state_t *s, const char *bytes, int len) {
    if (s->text_len + len >= TEXT_CAP) {
        np_set_status(s, "Buffer full!");
        return;
    }
    memmove(s->text + s->cursor + len, s->text + s->cursor,
            s->text_len - s->cursor);
    memcpy(s->text + s->cursor, bytes, len);
    s->text_len += len;
    s->cursor += len;
    s->text[s->text_len] = '\0';
    s->dirty = true;
    np_reindex(s);
    np_scroll_to_cursor(s);
}

static void np_backspace(teditor_state_t *s) {
    if (s->cursor == 0)
        return;
    memmove(s->text + s->cursor - 1, s->text + s->cursor,
            s->text_len - s->cursor);
    s->text_len--;
    s->cursor--;
    s->text[s->text_len] = '\0';
    s->dirty = true;
    np_reindex(s);
    np_scroll_to_cursor(s);
}

static void np_delete_fwd(teditor_state_t *s) {
    if (s->cursor >= s->text_len)
        return;
    memmove(s->text + s->cursor, s->text + s->cursor + 1,
            s->text_len - s->cursor - 1);
    s->text_len--;
    s->text[s->text_len] = '\0';
    s->dirty = true;
    np_reindex(s);
}

static bool np_load(teditor_state_t *s, const char *path) {
    fat16_file_t f;
    if (!fat16_open(path, &f))
        return false;
    int n = fat16_read(&f, s->text, TEXT_CAP - 1);
    fat16_close(&f);
    if (n < 0)
        n = 0;
    s->text_len = n;
    s->text[n] = '\0';
    s->cursor = 0;
    s->scroll_col = 0;
    s->dirty = false;
    np_reindex(s);
    np_scroll_to_cursor(s);
    return true;
}

static bool np_save(teditor_state_t *s, const char *path) {
    dir_entry_t de;
    if (fat16_find(path, &de))
        fat16_delete(path);
    fat16_file_t f;
    if (!fat16_create(path, &f))
        return false;
    int written = fat16_write(&f, s->text, s->text_len);
    fat16_close(&f);
    if (written < s->text_len)
        return false;
    s->dirty = false;
    return true;
}

static teditor_state_t *active_teditor(void) {
    window *fw = wm_focused_window();
    if (!fw)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &teditor_app)
            continue;
        teditor_state_t *s = (teditor_state_t *)a->state;
        if (s->win == fw)
            return s;
    }
    return NULL;
}

static bool teditor_close_cb(window *w);

static void menu_new(void) {
    teditor_state_t *s = active_teditor();
    if (!s)
        return;
    s->text[0] = '\0';
    s->text_len = 0;
    s->cursor = 0;
    s->scroll_col = 0;
    s->dirty = false;
    s->filepath[0] = '\0';
    s->filename[0] = '\0';
    s->win->title = "TEditor";
    np_reindex(s);
    np_set_status(s, "New document.");
}

static void menu_open(void) {
    teditor_state_t *s = active_teditor();
    if (!s)
        return;
    s->fname_buf[0] = '\0';
    s->fname_len = 0;
    s->fname_mode = FNAME_MODE_OPEN;
}

static void menu_save(void) {
    teditor_state_t *s = active_teditor();
    if (!s)
        return;
    if (s->filepath[0] == '\0') {
        s->fname_buf[0] = '\0';
        s->fname_len = 0;
        s->fname_mode = FNAME_MODE_SAVEAS;
        return;
    }
    np_set_status(s, np_save(s, s->filepath) ? "Saved." : "Save failed!");
}

static void menu_saveas(void) {
    teditor_state_t *s = active_teditor();
    if (!s)
        return;
    s->fname_buf[0] = '\0';
    s->fname_len = 0;
    s->fname_mode = FNAME_MODE_SAVEAS;
}

static void menu_close_np(void) {
    teditor_state_t *s = active_teditor();
    if (!s)
        return;
    teditor_close_cb(s->win);
}

static void on_about_np(void) {
    modal_show(MODAL_INFO, "About TEditor", "TEditor v1.1\nASMOS Text Editor",
               NULL, NULL);
}

static void get_layout(const teditor_state_t *s, int *text_x, int *text_y,
                       int *text_w, int *text_h, int *vsb_x, int *vsb_y,
                       int *vsb_h, int *hsb_x, int *hsb_y, int *hsb_w,
                       int *status_y) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16;
    int ww = s->win->w - 2;
    int wh = s->win->h - 16;

    *status_y = wy + wh - STATUS_H - 2;
    int below_text = STATUS_H + 3 + SCROLLBAR_H + 1;

    *text_x = wx + MARGIN_X;
    *text_y = wy + MARGIN_Y;
    *text_w = ww - MARGIN_X * 2 - SCROLLBAR_W - 2;
    *text_h = wh - MARGIN_Y * 2 - below_text;

    *vsb_x = wx + ww - SCROLLBAR_W - 1;
    *vsb_y = wy + MARGIN_Y;
    *vsb_h = *text_h;

    *hsb_x = wx;
    *hsb_y = *status_y - SCROLLBAR_H - 2;
    *hsb_w = ww;
}

static void np_draw(window *win, void *ud) {
    teditor_state_t *s = (teditor_state_t *)ud;
    if (!s)
        return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;

    int text_x, text_y, text_w, text_h;
    int vsb_x, vsb_y, vsb_h;
    int hsb_x, hsb_y, hsb_w;
    int status_y;
    get_layout(s, &text_x, &text_y, &text_w, &text_h, &vsb_x, &vsb_y, &vsb_h,
               &hsb_x, &hsb_y, &hsb_w, &status_y);

    fill_rect(wx, wy, ww, wh, WHITE);

    int cursor_line = np_line_of(s, s->cursor);
    int cursor_col = np_col_of(s, s->cursor);
    int max_vis_chars = text_w / CHAR_W;
    if (max_vis_chars < 1)
        max_vis_chars = 1;

    for (int row = 0; row < s->visible_rows; row++) {
        int ln = s->scroll_line + row;
        if (ln >= s->line_count)
            break;

        int py = text_y + row * LINE_H;

        if (ln == cursor_line)
            fill_rect(wx + 1, py, ww - SCROLLBAR_W - 3, LINE_H, LIGHT_YELLOW);

        int ls = s->line_start[ln];
        int le =
            (ln + 1 < s->line_count) ? s->line_start[ln + 1] - 1 : s->text_len;
        int ll = le - ls;
        if (ll < 0)
            ll = 0;

        int src_start = s->scroll_col;
        if (src_start > ll)
            src_start = ll;
        int avail = ll - src_start;

        char draw_buf[MAX_COLS + 1];
        int to_copy = avail < max_vis_chars ? avail : max_vis_chars;
        if (to_copy > MAX_COLS)
            to_copy = MAX_COLS;
        if (to_copy > 0)
            memcpy(draw_buf, s->text + ls + src_start, to_copy);
        draw_buf[to_copy] = '\0';

        draw_string(text_x, py, draw_buf, BLACK, 2);

        if (ln == cursor_line) {
            int vis_col = cursor_col - s->scroll_col;
            if (vis_col >= 0 && vis_col <= max_vis_chars) {
                int cx = text_x + vis_col * CHAR_W;
                extern volatile uint32_t pit_ticks;
                if ((pit_ticks / 50) % 2 == 0)
                    fill_rect(cx, py, 1, LINE_H, BLACK);
            }
        }
    }

    fill_rect(vsb_x, vsb_y, SCROLLBAR_W, vsb_h, LIGHT_GRAY);
    draw_rect(vsb_x, vsb_y, SCROLLBAR_W, vsb_h, DARK_GRAY);

    if (s->line_count > s->visible_rows && s->line_count > 0) {
        int max_scroll = s->line_count - s->visible_rows;
        int thumb_h = (s->visible_rows * vsb_h) / s->line_count;
        if (thumb_h < 4)
            thumb_h = 4;
        int thumb_range = vsb_h - thumb_h;
        int thumb_y = vsb_y;
        if (max_scroll > 0)
            thumb_y += (s->scroll_line * thumb_range) / max_scroll;
        fill_rect(vsb_x + 1, thumb_y, SCROLLBAR_W - 2, thumb_h, DARK_GRAY);
    }

    fill_rect(hsb_x, hsb_y, hsb_w, SCROLLBAR_H, LIGHT_GRAY);
    draw_rect(hsb_x, hsb_y, hsb_w, SCROLLBAR_H, DARK_GRAY);
    draw_line(hsb_x, hsb_y - 1, hsb_x + hsb_w - 1, hsb_y - 1, DARK_GRAY);

    if (s->max_line_len > max_vis_chars && s->max_line_len > 0) {
        int max_h_scroll = s->max_line_len - max_vis_chars;
        int track_w = hsb_w - 4;
        int thumb_w = (max_vis_chars * track_w) / s->max_line_len;
        if (thumb_w < 4)
            thumb_w = 4;
        int thumb_range = track_w - thumb_w;
        int thumb_x = hsb_x + 2;
        if (max_h_scroll > 0)
            thumb_x += (s->scroll_col * thumb_range) / max_h_scroll;
        fill_rect(thumb_x, hsb_y + 1, thumb_w, SCROLLBAR_H - 2, DARK_GRAY);
    }

    fill_rect(wx, status_y - 1, ww, STATUS_H + 3, LIGHT_GRAY);
    draw_line(wx, status_y - 1, wx + ww - 1, status_y - 1, DARK_GRAY);

    char stat[80];
    if (s->status_timer > 0) {
        strncpy(stat, s->status, 79);
        stat[79] = '\0';
    } else {
        sprintf(stat, "Ln %d  Col %d  %s%s", cursor_line + 1, cursor_col + 1,
                s->dirty ? "* " : "",
                s->filename[0] ? s->filename : "Untitled");
    }
    draw_string(wx + 3, status_y + 1, stat, BLACK, 2);

    if (s->fname_mode != FNAME_MODE_NONE) {
        int bw = ww - 8;
        int bh = 40;
        int bx = wx + 4;
        int by = wy + wh / 2 - bh / 2;

        fill_rect(bx + 3, by + 3, bw, bh, BLACK);
        fill_rect(bx, by, bw, bh, LIGHT_GRAY);
        draw_rect(bx, by, bw, bh, BLACK);

        fill_rect(bx, by, bw, 11, DARK_GRAY);
        const char *dlg_title =
            (s->fname_mode == FNAME_MODE_OPEN) ? "Open File" : "Save As";
        draw_string(bx + 4, by + 3, (char *)dlg_title, WHITE, 2);

        draw_string(bx + 4, by + 15, "Path:", DARK_GRAY, 2);

        int field_x = bx + 32;
        int field_w = bw - 36;
        fill_rect(field_x, by + 14, field_w, 10, WHITE);
        draw_rect(field_x, by + 14, field_w, 10, BLACK);

        int field_chars = (field_w - 4) / CHAR_W;
        if (field_chars < 1)
            field_chars = 1;
        const char *show = s->fname_buf;
        int slen = s->fname_len;
        if (slen > field_chars) {
            show = s->fname_buf + (slen - field_chars);
        }
        draw_string(field_x + 2, by + 15, (char *)show, BLACK, 2);

        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 50) % 2 == 0) {
            int vis_len = slen < field_chars ? slen : field_chars;
            int cx = field_x + 2 + vis_len * CHAR_W;
            int max_cx = field_x + field_w - 2;
            if (cx <= max_cx)
                draw_string(cx, by + 15, "|", BLACK, 2);
        }

        fill_rect(bx + 4, by + 28, 30, 8, LIGHT_BLUE);
        draw_rect(bx + 4, by + 28, 30, 8, BLACK);
        draw_string(bx + 11, by + 29, "OK", WHITE, 2);

        fill_rect(bx + 38, by + 28, 40, 8, DARK_GRAY);
        draw_rect(bx + 38, by + 28, 40, 8, BLACK);
        draw_string(bx + 41, by + 29, "Cancel", WHITE, 2);
    }
}

static void commit_fname(teditor_state_t *s) {
    const char *last_sep = strrchr(s->fname_buf, '/');
    const char *base = last_sep ? last_sep + 1 : s->fname_buf;
    int base_len = (int)strlen(base);

    bool has_dot = (strchr(base, '.') != NULL);
    if (!has_dot && base_len > 0 && base_len <= 8) {
        if (s->fname_len + 4 < FNAME_BUF_CAP) {
            s->fname_buf[s->fname_len++] = '.';
            s->fname_buf[s->fname_len++] = 'T';
            s->fname_buf[s->fname_len++] = 'X';
            s->fname_buf[s->fname_len++] = 'T';
            s->fname_buf[s->fname_len] = '\0';
        }
        last_sep = strrchr(s->fname_buf, '/');
        base = last_sep ? last_sep + 1 : s->fname_buf;
    }

    int fm = s->fname_mode;
    s->fname_mode = FNAME_MODE_NONE;

    strncpy(s->filepath, s->fname_buf, FNAME_BUF_CAP - 1);
    s->filepath[FNAME_BUF_CAP - 1] = '\0';

    strncpy(s->filename, base, FNAME_BUF_CAP - 1);
    s->filename[FNAME_BUF_CAP - 1] = '\0';

    s->win->title = s->filename;

    if (fm == FNAME_MODE_OPEN) {
        if (np_load(s, s->filepath)) {
            np_set_status(s, "Opened.");
        } else {
            s->filepath[0] = '\0';
            s->filename[0] = '\0';
            s->win->title = "TEditor";
            np_set_status(s, "File not found.");
        }
    } else {
        np_set_status(s, np_save(s, s->filepath) ? "Saved." : "Save failed!");
    }

    np_draw(s->win, s);
}

static bool teditor_close_cb(window *w) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &teditor_app)
            continue;
        teditor_state_t *s = (teditor_state_t *)a->state;
        if (s->win == w) {
            os_quit_app(a);
            return true;
        }
    }
    return true;
}

static void on_file_close_np(void) {
    teditor_state_t *s = active_teditor();
    if (!s)
        return;
    teditor_close_cb(s->win);
}

static void teditor_init(void *state) {
    teditor_state_t *s = (teditor_state_t *)state;

    const window_spec_t spec = {
        .x = teditor_DEFAULT_X,
        .y = teditor_DEFAULT_Y,
        .w = teditor_DEFAULT_W,
        .h = teditor_DEFAULT_H,
        .min_h = 70,
        .min_w = 160,
        .resizable = true,
        .title = "TEditor",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = WHITE,
        .visible = true,
        .on_close = teditor_close_cb,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;

    s->win->on_draw = np_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "New", menu_new);
    menu_add_item(file_menu, "Open", menu_open);
    menu_add_item(file_menu, "Save", menu_save);
    menu_add_item(file_menu, "Save As", menu_saveas);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", on_file_close_np);
    menu_add_item(file_menu, "About", on_about_np);

    s->text[0] = '\0';
    s->text_len = 0;
    s->cursor = 0;
    s->scroll_col = 0;
    s->dirty = false;
    s->fname_mode = FNAME_MODE_NONE;
    np_reindex(s);

    int wh = s->win->h - 16;
    int text_h = wh - MARGIN_Y * 2 - STATUS_H - 3 - SCROLLBAR_H - 1 - MARGIN_Y;
    s->visible_rows = text_h / LINE_H;
    if (s->visible_rows < 1)
        s->visible_rows = 1;
}

static void changecursor(void *state) {
    teditor_state_t *s = (teditor_state_t *)state;
    if (!s->win || !s->win->visible)
        return;

    if (s->win != wm_focused_window()) {
        CHOSEN_CURSOR = 0;
        return;
    }

    if (mouse.x >= s->win->x &&
        mouse.x <= s->win->x + s->win->w - SCROLLBAR_W &&
        mouse.y >= s->win->y + TITLEBAR_H &&
        mouse.y < s->win->y + s->win->h - SCROLLBAR_H)
        CHOSEN_CURSOR = 3;
    else
        CHOSEN_CURSOR = 0;
}

static void teditor_on_frame(void *state) {
    teditor_state_t *s = (teditor_state_t *)state;
    if (!s->win || !s->win->visible)
        return;

    {
        int wh = s->win->h - 16;
        int text_h =
            wh - MARGIN_Y * 2 - STATUS_H - 3 - SCROLLBAR_H - 1 - MARGIN_Y;
        s->visible_rows = text_h / LINE_H;
        if (s->visible_rows < 1)
            s->visible_rows = 1;
    }

    if (s->status_timer > 0)
        s->status_timer--;

    // changecursor(state);

    if (s->fname_mode != FNAME_MODE_NONE) {
        if (kb.key_pressed) {
            if (kb.last_scancode == ESC) {
                s->fname_mode = FNAME_MODE_NONE;
            } else if (kb.last_scancode == ENTER && s->fname_len > 0) {
                commit_fname(s);
                return;
            } else if (kb.last_scancode == BACKSPACE) {
                if (s->fname_len > 0)
                    s->fname_buf[--s->fname_len] = '\0';
            } else if (kb.last_char >= 32 && kb.last_char < 127 &&
                       s->fname_len < FNAME_BUF_CAP - 1) {
                char c = kb.last_char;
                bool ok = (c == '/' || c == '.' || (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '!' || c == '#' || c == '$' || c == '%' ||
                           c == '&' || c == '\'' || c == '(' || c == ')' ||
                           c == '-' || c == '@' || c == '^' || c == '_' ||
                           c == '`' || c == '{' || c == '}' || c == '~');
                if (ok) {
                    s->fname_buf[s->fname_len++] = c;
                    s->fname_buf[s->fname_len] = '\0';
                }
            }
        }

        if (mouse.left_clicked) {
            int wx2 = s->win->x + 1;
            int wy2 = s->win->y + MENUBAR_H + 16;
            int ww2 = s->win->w - 2;
            int wh2 = s->win->h - 16;
            int bw = ww2 - 8;
            int bh = 40;
            int bx = wx2 + 4;
            int by = wy2 + wh2 / 2 - bh / 2;

            bool ok_hit = (mouse.x >= bx + 4 && mouse.x < bx + 34 &&
                           mouse.y >= by + 28 && mouse.y < by + 36);
            bool can_hit = (mouse.x >= bx + 38 && mouse.x < bx + 78 &&
                            mouse.y >= by + 28 && mouse.y < by + 36);

            if (can_hit) {
                s->fname_mode = FNAME_MODE_NONE;
            } else if (ok_hit && s->fname_len > 0) {
                commit_fname(s);
                return;
            }
        }

        np_draw(s->win, s);
        return;
    }

    int text_x, text_y, text_w, text_h;
    int vsb_x, vsb_y, vsb_h;
    int hsb_x, hsb_y, hsb_w;
    int status_y;
    get_layout(s, &text_x, &text_y, &text_w, &text_h, &vsb_x, &vsb_y, &vsb_h,
               &hsb_x, &hsb_y, &hsb_w, &status_y);

    int max_vis_chars = text_w / CHAR_W;
    if (max_vis_chars < 1)
        max_vis_chars = 1;

    if ((mouse.left_clicked || mouse.left) && mouse.x >= vsb_x &&
        mouse.x < vsb_x + SCROLLBAR_W && mouse.y >= vsb_y &&
        mouse.y < vsb_y + vsb_h && s->line_count > s->visible_rows) {
        int max_scroll = s->line_count - s->visible_rows;
        int new_scroll = ((mouse.y - vsb_y) * max_scroll) / vsb_h;
        if (new_scroll < 0)
            new_scroll = 0;
        if (new_scroll > max_scroll)
            new_scroll = max_scroll;
        s->scroll_line = new_scroll;
    }

    if ((mouse.left_clicked || mouse.left) && mouse.x >= hsb_x &&
        mouse.x < hsb_x + hsb_w && mouse.y >= hsb_y &&
        mouse.y < hsb_y + SCROLLBAR_H && s->max_line_len > max_vis_chars) {
        int max_h_scroll = s->max_line_len - max_vis_chars;
        int track_w = hsb_w - 4;
        int new_scroll = ((mouse.x - hsb_x - 2) * max_h_scroll) / track_w;
        if (new_scroll < 0)
            new_scroll = 0;
        if (new_scroll > max_h_scroll)
            new_scroll = max_h_scroll;
        s->scroll_col = new_scroll;
    }

    if (mouse.left_clicked && mouse.x >= text_x && mouse.x < text_x + text_w &&
        mouse.y >= text_y && mouse.y < text_y + text_h) {
        int row = (mouse.y - text_y) / LINE_H;
        int ln = s->scroll_line + row;
        if (ln >= s->line_count)
            ln = s->line_count - 1;
        if (ln < 0)
            ln = 0;

        int col = (mouse.x - text_x) / CHAR_W + s->scroll_col;

        int ls = s->line_start[ln];
        int le =
            (ln + 1 < s->line_count) ? s->line_start[ln + 1] - 1 : s->text_len;
        int ll = le - ls;
        if (col > ll)
            col = ll;
        if (col < 0)
            col = 0;
        s->cursor = ls + col;
    }

    if (kb.key_pressed) {
        uint8_t sc = kb.last_scancode;

        if (sc == ENTER) {
            np_insert(s, "\n", 1);
        } else if (sc == BACKSPACE) {
            np_backspace(s);
        } else if (sc == DELETE) {
            np_delete_fwd(s);
        } else if (sc == UP_ARROW) {
            int ln = np_line_of(s, s->cursor);
            int col = np_col_of(s, s->cursor);
            if (ln > 0) {
                ln--;
                int ls = s->line_start[ln];
                int le = (ln + 1 < s->line_count) ? s->line_start[ln + 1] - 1
                                                  : s->text_len;
                int ll = le - ls;
                if (col > ll)
                    col = ll;
                s->cursor = ls + col;
                np_scroll_to_cursor(s);
            }
        } else if (sc == DOWN_ARROW) {
            int ln = np_line_of(s, s->cursor);
            int col = np_col_of(s, s->cursor);
            if (ln + 1 < s->line_count) {
                ln++;
                int ls = s->line_start[ln];
                int le = (ln + 1 < s->line_count) ? s->line_start[ln + 1] - 1
                                                  : s->text_len;
                int ll = le - ls;
                if (col > ll)
                    col = ll;
                s->cursor = ls + col;
                np_scroll_to_cursor(s);
            }
        } else if (sc == LEFT_ARROW) {
            if (s->cursor > 0) {
                s->cursor--;
                np_scroll_to_cursor(s);
            }
        } else if (sc == RIGHT_ARROW) {
            if (s->cursor < s->text_len) {
                s->cursor++;
                np_scroll_to_cursor(s);
            }
        } else if (sc == HOME) {
            int ln = np_line_of(s, s->cursor);
            s->cursor = s->line_start[ln];
            s->scroll_col = 0;
            np_scroll_to_cursor(s);
        } else if (sc == END) {
            int ln = np_line_of(s, s->cursor);
            int le = (ln + 1 < s->line_count) ? s->line_start[ln + 1] - 1
                                              : s->text_len;
            s->cursor = le;
            np_scroll_to_cursor(s);
        } else if (sc == PAGE_UP) {
            s->scroll_line -= s->visible_rows;
            if (s->scroll_line < 0)
                s->scroll_line = 0;
            if (np_line_of(s, s->cursor) < s->scroll_line)
                s->cursor = s->line_start[s->scroll_line];
        } else if (sc == PAGE_DOWN) {
            int max_scroll = s->line_count - s->visible_rows;
            if (max_scroll < 0)
                max_scroll = 0;
            s->scroll_line += s->visible_rows;
            if (s->scroll_line > max_scroll)
                s->scroll_line = max_scroll;
            int bot = s->scroll_line + s->visible_rows - 1;
            if (bot >= s->line_count)
                bot = s->line_count - 1;
            if (np_line_of(s, s->cursor) > bot)
                s->cursor = s->line_start[bot];
        } else if (sc == TAB) {
            np_insert(s, "    ", 4);
        } else if (kb.last_char >= 32 && kb.last_char < 127) {
            char c = kb.last_char;
            np_insert(s, &c, 1);
        }

        {
            int col = np_col_of(s, s->cursor);
            if (col < s->scroll_col)
                s->scroll_col = col;
            if (col >= s->scroll_col + max_vis_chars)
                s->scroll_col = col - max_vis_chars + 1;
            if (s->scroll_col < 0)
                s->scroll_col = 0;
        }
    }

    np_draw(s->win, s);
}

static void teditor_destroy(void *state) {
    CHOSEN_CURSOR = 0;
    teditor_state_t *s = (teditor_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor teditor_app = {
    .name = "TEDITOR",
    .state_size = sizeof(teditor_state_t),
    .init = teditor_init,
    .on_frame = teditor_on_frame,
    .destroy = teditor_destroy,
};
