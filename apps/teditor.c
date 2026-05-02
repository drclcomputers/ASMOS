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
#define SCROLLBAR_W 8
#define SCROLLBAR_H 8
#define STATUS_H 8

#define MAX_LINES 65536            // absolute hard limit
#define MAX_COLS 400               // draw buffer max (stack variable)
#define TEXT_MAX_CAP (1024 * 1024) // 1 MB hard ceiling for text

#define FNAME_BUF_CAP 256

#define FNAME_MODE_NONE 0
#define FNAME_MODE_OPEN 1
#define FNAME_MODE_SAVEAS 2

#define TEXT_INIT_CAP 4096
#define LINE_INIT_CAP 256

typedef struct {
    window *win;

    char *text;
    int text_len;
    int text_cap;

    int cursor;

    int scroll_line;
    int visible_rows;

    int scroll_col;
    int max_line_len;

    int *line_start;
    int line_count;
    int line_cap;

    bool dirty;

    char filepath[FNAME_BUF_CAP];
    char filename[FNAME_BUF_CAP];

    int fname_mode;
    char fname_buf[FNAME_BUF_CAP];
    int fname_len;

    char status[64];
    uint32_t status_timer;

    int sel_anchor;
} teditor_state_t;

app_descriptor teditor_app;

/* ── Status ──────────────────────────────────────────────────── */
static void np_set_status(teditor_state_t *s, const char *msg) {
    strncpy(s->status, msg, 63);
    s->status[63] = '\0';
    s->status_timer = 300;
}

static bool np_ensure_text_cap(teditor_state_t *s, int needed) {
    while (needed >= s->text_cap) {
        int new_cap = s->text_cap * 2;
        if (new_cap > TEXT_MAX_CAP)
            new_cap = TEXT_MAX_CAP;
        if (new_cap <= needed)
            new_cap = needed + 4096;
        if (new_cap > TEXT_MAX_CAP)
            new_cap = TEXT_MAX_CAP;
        char *new_text = krealloc(s->text, new_cap);
        if (!new_text) {
            np_set_status(s, "Out of memory (text)");
            return false;
        }
        s->text = new_text;
        s->text_cap = new_cap;
    }
    return true;
}

static bool np_ensure_line_cap(teditor_state_t *s, int count) {
    while (count > s->line_cap) {
        if (s->line_cap >= MAX_LINES) {
            np_set_status(s, "Line limit reached");
            return false;
        }
        int new_cap = s->line_cap * 2;
        if (new_cap > MAX_LINES)
            new_cap = MAX_LINES;
        int *new_arr = krealloc(s->line_start, new_cap * sizeof(int));
        if (!new_arr) {
            np_set_status(s, "Out of memory (lines)");
            return false;
        }
        s->line_start = new_arr;
        s->line_cap = new_cap;
    }
    return true;
}

/* ── Line indexing ───────────────────────────────────────────── */
static void np_reindex(teditor_state_t *s) {
    s->line_count = 0;
    s->max_line_len = 0;
    s->line_start[s->line_count++] = 0;

    int cur_len = 0;
    for (int i = 0; i < s->text_len; i++) {
        if (s->text[i] == '\n') {
            if (cur_len > s->max_line_len)
                s->max_line_len = cur_len;
            cur_len = 0;
            if (!np_ensure_line_cap(s, s->line_count + 1))
                return;
            s->line_start[s->line_count++] = i + 1;
        } else {
            cur_len++;
        }
    }
    if (cur_len > s->max_line_len)
        s->max_line_len = cur_len;
}

/* ── Binary search for line of a character position ──────────── */
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

/* ── Scrolling ───────────────────────────────────────────────── */
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

/* ── Editing operations ──────────────────────────────────────── */
static void np_insert(teditor_state_t *s, const char *bytes, int len) {
    if (!np_ensure_text_cap(s, s->text_len + len + 1))
        return;

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

/* ── Selection ───────────────────────────────────────────────── */
static void np_get_sel_range(teditor_state_t *s, int *start, int *end) {
    if (s->sel_anchor < 0) {
        *start = *end = -1;
        return;
    }
    int a = s->sel_anchor, b = s->cursor;
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    *start = a;
    *end = b;
}

static void np_copy_selection(teditor_state_t *s) {
    int st, en;
    np_get_sel_range(s, &st, &en);
    if (st >= 0 && en > st) {
        char tmp[512];
        int len = en - st;
        if (len > 511)
            len = 511;
        memcpy(tmp, s->text + st, len);
        tmp[len] = '\0';
        clipboard_set_text(tmp, len);
    }
}

static void np_delete_selection(teditor_state_t *s) {
    int st, en;
    np_get_sel_range(s, &st, &en);
    if (st >= 0 && en > st) {
        int len = en - st;
        memmove(s->text + st, s->text + en, s->text_len - en);
        s->text_len -= len;
        s->text[s->text_len] = '\0';
        s->cursor = st;
        s->sel_anchor = -1;
        s->dirty = true;
        np_reindex(s);
        np_scroll_to_cursor(s);
    }
}

static void np_select_all(teditor_state_t *s) {
    s->sel_anchor = 0;
    s->cursor = s->text_len;
    np_scroll_to_cursor(s);
}

/* ── File I/O ────────────────────────────────────────────────── */
static bool np_load(teditor_state_t *s, const char *path) {
    fs_file_t f;
    if (!fs_open(path, &f))
        return false;

    s->text_len = 0;
    s->text[0] = '\0';

#define LOAD_CHUNK 4096
    for (;;) {
        if (!np_ensure_text_cap(s, s->text_len + LOAD_CHUNK))
            break;

        int space = s->text_cap - s->text_len - 1;
        if (space <= 0)
            break;
        int chunk = space < LOAD_CHUNK ? space : LOAD_CHUNK;
        int n = fs_read(&f, s->text + s->text_len, chunk);
        if (n <= 0)
            break;
        s->text_len += n;
        s->text[s->text_len] = '\0';
    }
    fs_close(&f);

    s->cursor = 0;
    s->scroll_col = 0;
    s->dirty = false;
    np_reindex(s);
    np_scroll_to_cursor(s);
    return true;
}

void teditor_open_file(const char *path) {
    teditor_state_t *target = NULL;
    app_instance_t *inst = NULL;

    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &teditor_app)
            continue;
        teditor_state_t *s = (teditor_state_t *)a->state;
        if (!s->dirty && s->text_len == 0) {
            target = s;
            break;
        }
    }

    if (!target) {
        inst = os_launch_app(&teditor_app);
        if (!inst)
            return;
        target = (teditor_state_t *)inst->state;
    }

    if (!target || !target->win)
        return;
    strncpy(target->filepath, path, FNAME_BUF_CAP - 1);
    target->filepath[FNAME_BUF_CAP - 1] = '\0';
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(target->filename, base, FNAME_BUF_CAP - 1);
    target->filename[FNAME_BUF_CAP - 1] = '\0';
    target->win->title = target->filename;
    np_load(target, path);
    wm_focus(target->win);
}

static bool np_save(teditor_state_t *s, const char *path) {
    dir_entry_t de;
    if (fs_find(path, &de))
        fs_delete(path);
    fs_file_t f;
    if (!fs_create(path, &f))
        return false;
    int written = fs_write(&f, s->text, s->text_len);
    fs_close(&f);
    if (written < s->text_len)
        return false;
    s->dirty = false;
    return true;
}

/* ── Menu callbacks ──────────────────────────────────────────── */
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

static void menu_new(void) { os_launch_app(&teditor_app); }

static void menu_open(void) {
    teditor_state_t *s = active_teditor();
    if (!s)
        return;
    if (s->dirty || s->text_len > 0) {
        app_instance_t *inst = os_launch_app(&teditor_app);
        if (!inst)
            return;
        teditor_state_t *ns = (teditor_state_t *)inst->state;
        if (!ns || !ns->win)
            return;
        ns->fname_buf[0] = '\0';
        ns->fname_len = 0;
        ns->fname_mode = FNAME_MODE_OPEN;
        return;
    }
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
    modal_show(MODAL_INFO, "About TEditor", "TEditor v1.2\nDynamic Tiny Editor",
               NULL, NULL);
}

/* ── Layout & drawing ───────────────────────────────────────────────────── */
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
            fill_rect(wx + 1, py, ww - SCROLLBAR_W - 3, LINE_H, LIGHT_CYAN);

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
        int to_copy = (avail < max_vis_chars) ? avail : max_vis_chars;
        if (to_copy > MAX_COLS)
            to_copy = MAX_COLS;
        if (to_copy > 0)
            memcpy(draw_buf, s->text + ls + src_start, to_copy);
        draw_buf[to_copy] = '\0';

        if (s->sel_anchor >= 0) {
            int st, en;
            np_get_sel_range(s, &st, &en);
            if (st < en) {
                int line_start_pos = s->line_start[ln];
                int line_end_pos = (ln + 1 < s->line_count)
                                       ? s->line_start[ln + 1] - 1
                                       : s->text_len;
                if (line_end_pos > line_start_pos && st < line_end_pos &&
                    en > line_start_pos) {
                    int sel_st = (st > line_start_pos) ? st : line_start_pos;
                    int sel_en = (en < line_end_pos) ? en : line_end_pos;
                    if (sel_st < sel_en) {
                        int vis_st_col =
                            sel_st - line_start_pos - s->scroll_col;
                        int vis_en_col =
                            sel_en - line_start_pos - s->scroll_col;
                        if (vis_st_col < 0)
                            vis_st_col = 0;
                        if (vis_en_col > max_vis_chars)
                            vis_en_col = max_vis_chars;
                        if (vis_st_col < vis_en_col) {
                            int x0 = text_x + vis_st_col * CHAR_W;
                            int x1 = text_x + vis_en_col * CHAR_W;
                            fill_rect(x0, py, x1 - x0, LINE_H, LIGHT_GRAY);
                        }
                    }
                }
            }
        }

        draw_string(text_x, py, draw_buf, BLACK, 2);

        /* cursor */
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

    /* scrollbars & status bar */
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

    /* filename dialog */
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
        if (slen > field_chars)
            show = s->fname_buf + (slen - field_chars);
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

/* ── Filename dialog commit ──────────────────────────────────── */
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
        if (np_load(s, s->filepath))
            np_set_status(s, "Opened.");
        else {
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

/* ── Close handling ──────────────────────────────────────────── */
static app_instance_t *s_pending_close = NULL;

static void teditor_confirm_close(void) {
    if (s_pending_close) {
        os_quit_app(s_pending_close);
        s_pending_close = NULL;
    }
}

static bool teditor_close_cb(window *w) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &teditor_app)
            continue;
        teditor_state_t *s = (teditor_state_t *)a->state;
        if (s->win != w)
            continue;
        if (s->dirty) {
            s_pending_close = a;
            modal_show(MODAL_CONFIRM, "Unsaved Changes",
                       "Close without saving?", teditor_confirm_close, NULL);
            return true;
        }
        os_quit_app(a);
        return true;
    }
    return true;
}

static void on_file_close_np(void) {
    teditor_state_t *s = active_teditor();
    if (!s)
        return;
    teditor_close_cb(s->win);
}

/* ── Init / Destroy ──────────────────────────────────────────── */
static void teditor_init(void *state) {
    teditor_state_t *s = (teditor_state_t *)state;

    /* window */
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

    /* menus */
    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "New", menu_new);
    menu_add_item(file_menu, "Open", menu_open);
    menu_add_item(file_menu, "Save", menu_save);
    menu_add_item(file_menu, "Save As", menu_saveas);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", on_file_close_np);
    menu_add_item(file_menu, "About", on_about_np);

    /* allocate dynamic text buffer */
    s->text_cap = TEXT_INIT_CAP;
    s->text = kmalloc(s->text_cap);
    if (!s->text) {
        ERR_WARN_REPORT(ERR_WM_ALLOC, "teditor: text buffer");
        return;
    }
    s->text[0] = '\0';
    s->text_len = 0;

    /* allocate dynamic line-start array */
    s->line_cap = LINE_INIT_CAP;
    s->line_start = kmalloc(s->line_cap * sizeof(int));
    if (!s->line_start) {
        kfree(s->text);
        s->text = NULL;
        ERR_WARN_REPORT(ERR_WM_ALLOC, "teditor: line array");
        return;
    }

    s->cursor = 0;
    s->scroll_col = 0;
    s->sel_anchor = -1;
    s->dirty = false;
    s->fname_mode = FNAME_MODE_NONE;
    np_reindex(s);

    int wh = s->win->h - 16;
    int text_h = wh - MARGIN_Y * 2 - STATUS_H - 3 - SCROLLBAR_H - 1 - MARGIN_Y;
    s->visible_rows = text_h / LINE_H;
    if (s->visible_rows < 1)
        s->visible_rows = 1;
}

static void teditor_destroy(void *state) {
    teditor_state_t *s = (teditor_state_t *)state;
    if (s->text) {
        kfree(s->text);
        s->text = NULL;
    }
    if (s->line_start) {
        kfree(s->line_start);
        s->line_start = NULL;
    }
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
    CHOSEN_CURSOR = 0;
}

/* ── Frame handler ───────────────────────────────────────────────── */
static void teditor_on_frame(void *state) {
    teditor_state_t *s = (teditor_state_t *)state;
    if (!s->win || !s->win->visible)
        return;

    bool focused = window_is_focused(s->win);

    int wh = s->win->h - 16;
    int text_h = wh - MARGIN_Y * 2 - STATUS_H - 3 - SCROLLBAR_H - 1 - MARGIN_Y;
    s->visible_rows = text_h / LINE_H;
    if (s->visible_rows < 1)
        s->visible_rows = 1;

    if (s->status_timer > 0)
        s->status_timer--;

    /* filename mode */
    if (s->fname_mode != FNAME_MODE_NONE) {
        if (focused) {
            if (kb.key_pressed) {
                if (kb.ctrl_v && clipboard_has_text()) {
                    for (int i = 0; i < g_clipboard.text_len &&
                                    s->fname_len < FNAME_BUF_CAP - 1;
                         i++)
                        s->fname_buf[s->fname_len++] = g_clipboard.text[i];
                    s->fname_buf[s->fname_len] = '\0';
                    return;
                }
                if (kb.last_scancode == ESC)
                    s->fname_mode = FNAME_MODE_NONE;
                else if (kb.last_scancode == ENTER && s->fname_len > 0) {
                    commit_fname(s);
                    return;
                } else if (kb.last_scancode == BACKSPACE) {
                    if (s->fname_len > 0)
                        s->fname_buf[--s->fname_len] = '\0';
                } else if (kb.last_char >= 32 && kb.last_char < 127 &&
                           s->fname_len < FNAME_BUF_CAP - 1) {
                    char c = kb.last_char;
                    bool ok =
                        (c == '/' || c == '.' || (c >= 'A' && c <= 'Z') ||
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
                int wx2 = s->win->x + 1, wy2 = s->win->y + MENUBAR_H + 16,
                    ww2 = s->win->w - 2, wh2 = s->win->h - 16;
                int bw = ww2 - 8, bh = 40, bx = wx2 + 4,
                    by = wy2 + wh2 / 2 - bh / 2;
                if (mouse.x >= bx + 4 && mouse.x < bx + 34 &&
                    mouse.y >= by + 28 && mouse.y < by + 36) {
                    if (s->fname_len > 0)
                        commit_fname(s);
                } else if (mouse.x >= bx + 38 && mouse.x < bx + 78 &&
                           mouse.y >= by + 28 && mouse.y < by + 36) {
                    s->fname_mode = FNAME_MODE_NONE;
                }
            }
            np_draw(s->win, s);
        }
        return;
    }

    /* main editing logic */
    int text_x, text_y, text_w, vsb_x, vsb_y, vsb_h, hsb_x, hsb_y, hsb_w,
        status_y;
    get_layout(s, &text_x, &text_y, &text_w, &text_h, &vsb_x, &vsb_y, &vsb_h,
               &hsb_x, &hsb_y, &hsb_w, &status_y);
    int max_vis_chars = text_w / CHAR_W;
    if (max_vis_chars < 1)
        max_vis_chars = 1;

    if (focused) {
        if ((mouse.left_clicked || mouse.left) && mouse.x >= vsb_x &&
            mouse.x < vsb_x + SCROLLBAR_W && mouse.y >= vsb_y &&
            mouse.y < vsb_y + vsb_h && s->line_count > s->visible_rows) {
            int max_scroll = s->line_count - s->visible_rows;
            int ns = ((mouse.y - vsb_y) * max_scroll) / vsb_h;
            if (ns < 0)
                ns = 0;
            if (ns > max_scroll)
                ns = max_scroll;
            s->scroll_line = ns;
        }
        if ((mouse.left_clicked || mouse.left) && mouse.x >= hsb_x &&
            mouse.x < hsb_x + hsb_w && mouse.y >= hsb_y &&
            mouse.y < hsb_y + SCROLLBAR_H && s->max_line_len > max_vis_chars) {
            int max_h = s->max_line_len - max_vis_chars;
            int track_w = hsb_w - 4;
            int ns = ((mouse.x - hsb_x - 2) * max_h) / track_w;
            if (ns < 0)
                ns = 0;
            if (ns > max_h)
                ns = max_h;
            s->scroll_col = ns;
        }
        if (mouse.left_clicked && mouse.x >= text_x &&
            mouse.x < text_x + text_w && mouse.y >= text_y &&
            mouse.y < text_y + text_h) {
            int row = (mouse.y - text_y) / LINE_H;
            int ln = s->scroll_line + row;
            if (ln >= s->line_count)
                ln = s->line_count - 1;
            if (ln < 0)
                ln = 0;
            int col = (mouse.x - text_x) / CHAR_W + s->scroll_col;
            int ls = s->line_start[ln], le = (ln + 1 < s->line_count)
                                                 ? s->line_start[ln + 1] - 1
                                                 : s->text_len;
            int ll = le - ls;
            if (col > ll)
                col = ll;
            if (col < 0)
                col = 0;
            s->sel_anchor = -1;
            s->cursor = ls + col;
        }

        if (kb.key_pressed) {
            if (kb.ctrl_c) {
                np_copy_selection(s);
                np_draw(s->win, s);
                kb.key_pressed = false;
                return;
            }
            if (kb.ctrl_x) {
                np_copy_selection(s);
                np_delete_selection(s);
                np_draw(s->win, s);
                kb.key_pressed = false;
                return;
            }
            if (kb.ctrl_v) {
                if (clipboard_has_text()) {
                    np_delete_selection(s);
                    np_insert(s, g_clipboard.text, g_clipboard.text_len);
                }
                np_draw(s->win, s);
                kb.key_pressed = false;
                return;
            }
            if (kb.ctrl_s) {
                menu_save();
                np_draw(s->win, s);
                kb.key_pressed = false;
                return;
            }
            if (kb.ctrl_n) {
                menu_new();
                np_draw(s->win, s);
                kb.key_pressed = false;
                return;
            }
            if (kb.ctrl_o) {
                menu_open();
                np_draw(s->win, s);
                kb.key_pressed = false;
                return;
            }
            if (kb.ctrl_a) {
                np_select_all(s);
                np_draw(s->win, s);
                kb.key_pressed = false;
                return;
            }

            bool shift = kb.shift;
            uint8_t sc = kb.last_scancode;
            if (sc == UP_ARROW || sc == DOWN_ARROW || sc == LEFT_ARROW ||
                sc == RIGHT_ARROW || sc == HOME || sc == END || sc == PAGE_UP ||
                sc == PAGE_DOWN)
                if (!shift)
                    s->sel_anchor = -1;

            if (sc == ENTER)
                np_insert(s, "\n", 1);
            else if (sc == BACKSPACE)
                np_backspace(s);
            else if (sc == DELETE)
                np_delete_fwd(s);
            else if (sc == UP_ARROW) {
                if (shift && s->sel_anchor < 0)
                    s->sel_anchor = s->cursor;
                int ln = np_line_of(s, s->cursor),
                    col = np_col_of(s, s->cursor);
                if (ln > 0) {
                    ln--;
                    int ls = s->line_start[ln],
                        le = (ln + 1 < s->line_count)
                                 ? s->line_start[ln + 1] - 1
                                 : s->text_len;
                    if (col > le - ls)
                        col = le - ls;
                    s->cursor = ls + col;
                    np_scroll_to_cursor(s);
                }
            } else if (sc == DOWN_ARROW) {
                if (shift && s->sel_anchor < 0)
                    s->sel_anchor = s->cursor;
                int ln = np_line_of(s, s->cursor),
                    col = np_col_of(s, s->cursor);
                if (ln + 1 < s->line_count) {
                    ln++;
                    int ls = s->line_start[ln],
                        le = (ln + 1 < s->line_count)
                                 ? s->line_start[ln + 1] - 1
                                 : s->text_len;
                    if (col > le - ls)
                        col = le - ls;
                    s->cursor = ls + col;
                    np_scroll_to_cursor(s);
                }
            } else if (sc == LEFT_ARROW) {
                if (shift && s->sel_anchor < 0)
                    s->sel_anchor = s->cursor;
                if (s->cursor > 0) {
                    s->cursor--;
                    np_scroll_to_cursor(s);
                }
            } else if (sc == RIGHT_ARROW) {
                if (shift && s->sel_anchor < 0)
                    s->sel_anchor = s->cursor;
                if (s->cursor < s->text_len) {
                    s->cursor++;
                    np_scroll_to_cursor(s);
                }
            } else if (sc == HOME) {
                if (shift && s->sel_anchor < 0)
                    s->sel_anchor = s->cursor;
                int ln = np_line_of(s, s->cursor);
                s->cursor = s->line_start[ln];
                s->scroll_col = 0;
                np_scroll_to_cursor(s);
            } else if (sc == END) {
                if (shift && s->sel_anchor < 0)
                    s->sel_anchor = s->cursor;
                int ln = np_line_of(s, s->cursor);
                int le = (ln + 1 < s->line_count) ? s->line_start[ln + 1] - 1
                                                  : s->text_len;
                s->cursor = le;
                np_scroll_to_cursor(s);
            } else if (sc == PAGE_UP || sc == F5) {
                if (shift && s->sel_anchor < 0)
                    s->sel_anchor = s->cursor;
                s->scroll_line -= s->visible_rows;
                if (s->scroll_line < 0)
                    s->scroll_line = 0;
                if (np_line_of(s, s->cursor) < s->scroll_line)
                    s->cursor = s->line_start[s->scroll_line];
            } else if (sc == PAGE_DOWN || sc == F6) {
                if (shift && s->sel_anchor < 0)
                    s->sel_anchor = s->cursor;
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
            } else if (sc == TAB)
                np_insert(s, "    ", 4);
            else if (kb.last_char >= 32 && kb.last_char < 127) {
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
    }

    np_draw(s->win, s);
}

app_descriptor teditor_app = {
    .name = "TEDITOR",
    .state_size = sizeof(teditor_state_t),
    .init = teditor_init,
    .on_frame = teditor_on_frame,
    .destroy = teditor_destroy,
};
