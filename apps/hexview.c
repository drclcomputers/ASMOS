#include "os/api.h"

#define HEXVIEW_HEX_W 220
#define HEXVIEW_HEX_H 170
#define HEXVIEW_HEX_X 15
#define HEXVIEW_HEX_Y 15

#define HEXVIEW_ASC_W 110
#define HEXVIEW_ASC_H 170
#define HEXVIEW_ASC_X (HEXVIEW_HEX_X + HEXVIEW_HEX_W + 4)
#define HEXVIEW_ASC_Y HEXVIEW_HEX_Y

#define BYTES_PER_ROW 16
#define MAX_FILE_SIZE 65536

#define CHAR_W 5
#define LINE_H 8
#define MARGIN_X 4
#define MARGIN_Y 4
#define SCROLLBAR_W 8
#define SCROLLBAR_H 6
#define STATUS_H 8

#define HEX_ROW_CHARS (5 + BYTES_PER_ROW * 3)
#define HEX_ROW_PX (HEX_ROW_CHARS * CHAR_W)
#define ASC_CHAR_STEP (CHAR_W + 1)
#define ASC_ROW_PX (BYTES_PER_ROW * ASC_CHAR_STEP)

#define COL_BG BLACK
#define COL_ADDR CYAN
#define COL_HEX_EVEN WHITE
#define COL_HEX_ODD LIGHT_GRAY
#define COL_HEX_ZERO DARK_GRAY
#define COL_SEL_BG LIGHT_BLUE
#define COL_SEL_FG WHITE
#define COL_HEADER DARK_GRAY
#define COL_HEADER_TEXT YELLOW
#define COL_SEP DARK_GRAY
#define COL_STATUS_BG DARK_GRAY
#define COL_STATUS_FG LIGHT_CYAN
#define COL_ASCII_PRINT LIGHT_GREEN
#define COL_ASCII_CTRL DARK_GRAY
#define COL_ASCII_SEL_BG RED
#define COL_ASCII_SEL_FG WHITE
#define COL_HSB_BG DARK_GRAY
#define COL_HSB_THUMB LIGHT_GRAY

#define FNAME_BUF_CAP 256
#define FNAME_MODE_NONE 0
#define FNAME_MODE_OPEN 1

typedef struct {
    window *win_hex;
    window *win_asc;

    uint8_t *data;
    int data_len;

    int scroll_row;
    int visible_rows;

    int hscroll_hex;
    int hscroll_asc;

    int sel_byte;

    char filepath[FNAME_BUF_CAP];
    char filename[FNAME_BUF_CAP];

    int fname_mode;
    char fname_buf[FNAME_BUF_CAP];
    int fname_len;

    char status[64];
    uint32_t status_timer;
} hexview_state_t;

app_descriptor hexview_app;

/* ── helpers ──────────────────────────────────────────────────────────── */

static void hv_set_status(hexview_state_t *s, const char *msg) {
    strncpy(s->status, msg, 63);
    s->status[63] = '\0';
    s->status_timer = 300;
}

static int total_rows(const hexview_state_t *s) {
    if (s->data_len == 0)
        return 0;
    return (s->data_len + BYTES_PER_ROW - 1) / BYTES_PER_ROW;
}

static void hv_clamp_scroll(hexview_state_t *s) {
    int rows = total_rows(s);
    int ms = rows - s->visible_rows;
    if (ms < 0)
        ms = 0;
    if (s->scroll_row > ms)
        s->scroll_row = ms;
    if (s->scroll_row < 0)
        s->scroll_row = 0;
}

static void hv_scroll_to_sel(hexview_state_t *s) {
    if (s->sel_byte < 0)
        return;
    int row = s->sel_byte / BYTES_PER_ROW;
    if (row < s->scroll_row)
        s->scroll_row = row;
    if (row >= s->scroll_row + s->visible_rows)
        s->scroll_row = row - s->visible_rows + 1;
    hv_clamp_scroll(s);
}

static bool hv_load(hexview_state_t *s, const char *path) {
    fs_file_t f;
    if (!fs_open(path, &f))
        return false;
    uint32_t fsize = f.entry.file_size;
    if (fsize == 0) {
        fs_close(&f);
        if (s->data) {
            kfree(s->data);
            s->data = NULL;
        }
        s->data_len = 0;
        s->scroll_row = 0;
        s->sel_byte = -1;
        return true;
    }
    uint32_t cap = fsize > MAX_FILE_SIZE ? MAX_FILE_SIZE : fsize;
    uint8_t *buf = (uint8_t *)kmalloc(cap);
    if (!buf) {
        fs_close(&f);
        return false;
    }
    int got = fs_read(&f, buf, (int)cap);
    fs_close(&f);
    if (got < 0)
        got = 0;
    if (s->data)
        kfree(s->data);
    s->data = buf;
    s->data_len = got;
    s->scroll_row = 0;
    s->hscroll_hex = 0;
    s->hscroll_asc = 0;
    s->sel_byte = got > 0 ? 0 : -1;
    return true;
}

/* ── layout helpers ───────────────────────────────────────────────────── */
/*
 * hex window content area:
 *   hdr_y   = top of column-header strip
 *   data_y  = top of first data row
 *   data_h  = height available for data rows (clipped)
 *   vsb_*   = vertical scrollbar
 *   hsb_*   = horizontal scrollbar
 *   stat_y  = top of status bar
 *   clip_x  = left edge of clipped data area (= tx)
 *   clip_w  = width of clipped data area
 */
static void hex_layout(const hexview_state_t *s, int *clip_x, int *clip_w,
                       int *hdr_y, int *data_y, int *data_h, int *vsb_x,
                       int *vsb_y, int *vsb_h, int *hsb_x, int *hsb_y,
                       int *hsb_w, int *stat_y) {
    window *w = s->win_hex;
    int wx = w->x + 1;
    int wy = w->y + MENUBAR_H + 16;
    int ww = w->w - 2;
    int wh = w->h - 16;

    *stat_y = wy + wh - STATUS_H - 2;
    int bot = STATUS_H + 3 + SCROLLBAR_H + 1;
    int hdr = LINE_H + 2;

    *hdr_y = wy + MARGIN_Y;
    *data_y = wy + MARGIN_Y + hdr;
    *data_h = wh - MARGIN_Y * 2 - bot - hdr;
    if (*data_h < 0)
        *data_h = 0;

    *clip_x = wx + MARGIN_X;
    *clip_w = ww - MARGIN_X * 2 - SCROLLBAR_W - 2;
    if (*clip_w < 0)
        *clip_w = 0;

    *vsb_x = wx + ww - SCROLLBAR_W - 1;
    *vsb_y = *data_y;
    *vsb_h = *data_h;

    *hsb_y = *stat_y - SCROLLBAR_H - 2;
    *hsb_x = wx;
    *hsb_w = ww - SCROLLBAR_W - 1;
}

static void asc_layout(const hexview_state_t *s, int *clip_x, int *clip_w,
                       int *hdr_y, int *data_y, int *data_h, int *hsb_x,
                       int *hsb_y, int *hsb_w, int *stat_y) {
    window *w = s->win_asc;
    int wx = w->x + 1;
    int wy = w->y + MENUBAR_H + 16;
    int ww = w->w - 2;
    int wh = w->h - 16;

    *stat_y = wy + wh - STATUS_H - 2;
    int bot = STATUS_H + 3 + SCROLLBAR_H + 1;
    int hdr = LINE_H + 2;

    *hdr_y = wy + MARGIN_Y;
    *data_y = wy + MARGIN_Y + hdr;
    *data_h = wh - MARGIN_Y * 2 - bot - hdr;
    if (*data_h < 0)
        *data_h = 0;

    *clip_x = wx + MARGIN_X;
    *clip_w = ww - MARGIN_X * 2;
    if (*clip_w < 0)
        *clip_w = 0;

    *hsb_y = *stat_y - SCROLLBAR_H - 2;
    *hsb_x = wx;
    *hsb_w = ww;
}

/* ── draw a horizontal scrollbar ─────────────────────────────────────── */
static void draw_hsb(int bx, int by, int bw, int content_px, int scroll_px) {
    fill_rect(bx, by, bw, SCROLLBAR_H, COL_HSB_BG);
    draw_rect(bx, by, bw, SCROLLBAR_H, COL_SEP);
    if (content_px <= bw || content_px <= 0)
        return;
    int max_scroll = content_px - bw;
    if (max_scroll <= 0)
        return;
    int track_w = bw - 4;
    int thumb_w = (bw * track_w) / content_px;
    if (thumb_w < 6)
        thumb_w = 6;
    if (thumb_w > track_w)
        thumb_w = track_w;
    int thumb_range = track_w - thumb_w;
    int thumb_x =
        bx + 2 + (thumb_range > 0 ? (scroll_px * thumb_range) / max_scroll : 0);
    fill_rect(thumb_x, by + 1, thumb_w, SCROLLBAR_H - 2, COL_HSB_THUMB);
}

static int hsb_pick(int mx, int bx, int bw, int content_px) {
    if (content_px <= bw || content_px <= 0)
        return 0;
    int max_scroll = content_px - bw;
    int rel = mx - bx;
    if (rel < 0)
        rel = 0;
    if (rel > bw)
        rel = bw;
    int ns = (rel * max_scroll) / bw;
    if (ns < 0)
        ns = 0;
    if (ns > max_scroll)
        ns = max_scroll;
    return ns;
}

static void draw_nibble(int x, int y, uint8_t n, uint8_t col) {
    char c = n < 10 ? '0' + n : 'A' + n - 10;
    draw_char(x, y, c, col, 2);
}

/* ── draw hex window ──────────────────────────────────────────────────── */
static void hv_draw_hex(window *win, void *ud) {
    hexview_state_t *s = (hexview_state_t *)ud;
    if (!s)
        return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(wx, wy, ww, wh, COL_BG);

    int clip_x, clip_w, hdr_y, data_y, data_h;
    int vsb_x, vsb_y, vsb_h, hsb_x, hsb_y, hsb_w, stat_y;
    hex_layout(s, &clip_x, &clip_w, &hdr_y, &data_y, &data_h, &vsb_x, &vsb_y,
               &vsb_h, &hsb_x, &hsb_y, &hsb_w, &stat_y);

    int vrows = data_h / LINE_H;
    if (vrows < 1)
        vrows = 1;
    s->visible_rows = vrows;

    int max_hscroll_hex = HEX_ROW_PX - clip_w;
    if (max_hscroll_hex < 0)
        max_hscroll_hex = 0;
    if (s->hscroll_hex > max_hscroll_hex)
        s->hscroll_hex = max_hscroll_hex;
    if (s->hscroll_hex < 0)
        s->hscroll_hex = 0;

    fill_rect(wx, hdr_y - 1, ww - SCROLLBAR_W - 2, LINE_H + 2, COL_HEADER);
    {
        int draw_x = clip_x - s->hscroll_hex;
        int hx = draw_x;

        if (hx + 5 * CHAR_W > clip_x && hx < clip_x + clip_w)
            draw_string(hx, hdr_y + 1, "ADDR ", COL_HEADER_TEXT, 2);
        hx += 5 * CHAR_W;

        for (int b = 0; b < BYTES_PER_ROW; b++) {
            if (hx + 2 * CHAR_W > clip_x && hx < clip_x + clip_w) {
                char tmp[3];
                tmp[0] = '0';
                tmp[1] = "0123456789ABCDEF"[b];
                tmp[2] = '\0';
                uint8_t hcol = (b % 2 == 0) ? COL_HEX_EVEN : COL_HEX_ODD;
                draw_string(hx, hdr_y + 1, tmp, hcol, 2);
            }
            hx += 3 * CHAR_W;
        }
    }
    draw_line(wx, data_y - 1, wx + ww - SCROLLBAR_W - 2, data_y - 1, COL_SEP);

    int rows = total_rows(s);
    for (int row = 0; row < vrows; row++) {
        int logical = s->scroll_row + row;
        if (logical >= rows)
            break;

        int py = data_y + row * LINE_H;
        int base = logical * BYTES_PER_ROW;
        int cnt = s->data_len - base;
        if (cnt > BYTES_PER_ROW)
            cnt = BYTES_PER_ROW;

        int rx = clip_x - s->hscroll_hex;

        if (rx + 5 * CHAR_W > clip_x && rx < clip_x + clip_w) {
            uint16_t a = (uint16_t)base;
            char addr[6];
            addr[0] = "0123456789ABCDEF"[(a >> 12) & 0xF];
            addr[1] = "0123456789ABCDEF"[(a >> 8) & 0xF];
            addr[2] = "0123456789ABCDEF"[(a >> 4) & 0xF];
            addr[3] = "0123456789ABCDEF"[a & 0xF];
            addr[4] = ' ';
            addr[5] = '\0';
            draw_string(rx, py, addr, COL_ADDR, 2);
        }
        rx += 5 * CHAR_W;

        for (int b = 0; b < BYTES_PER_ROW; b++) {
            if (rx + 2 * CHAR_W > clip_x && rx < clip_x + clip_w) {
                if (b < cnt) {
                    int bi = base + b;
                    uint8_t byte = s->data[bi];
                    bool sel = (bi == s->sel_byte);

                    int sel_left = rx - 1;
                    int sel_right = rx + 2 * CHAR_W + 1;
                    if (sel && sel_right > clip_x &&
                        sel_left < clip_x + clip_w) {
                        int fl = sel_left < clip_x ? clip_x : sel_left;
                        int fr = sel_right > clip_x + clip_w ? clip_x + clip_w
                                                             : sel_right;
                        fill_rect(fl, py - 1, fr - fl, LINE_H + 1, COL_SEL_BG);
                    }

                    uint8_t col;
                    if (sel)
                        col = COL_SEL_FG;
                    else if (!byte)
                        col = COL_HEX_ZERO;
                    else if (b % 2 == 0)
                        col = COL_HEX_EVEN;
                    else
                        col = COL_HEX_ODD;

                    if (rx >= clip_x && rx + CHAR_W <= clip_x + clip_w)
                        draw_nibble(rx, py, (byte >> 4) & 0xF, col);
                    if (rx + CHAR_W >= clip_x &&
                        rx + 2 * CHAR_W <= clip_x + clip_w)
                        draw_nibble(rx + CHAR_W, py, byte & 0xF, col);
                }
            }
            rx += 3 * CHAR_W;
        }
    }

    if (clip_x > wx)
        fill_rect(wx, data_y - 2, clip_x - wx, data_h + 2, COL_BG);
    fill_rect(clip_x + clip_w, data_y - 2,
              ww - (clip_x + clip_w - wx) - SCROLLBAR_W, data_h + 2, COL_BG);

    fill_rect(vsb_x, vsb_y, SCROLLBAR_W, vsb_h, COL_HEADER);
    draw_rect(vsb_x, vsb_y, SCROLLBAR_W, vsb_h, COL_SEP);
    if (rows > vrows) {
        int ms = rows - vrows;
        int th2 = (vrows * vsb_h) / rows;
        if (th2 < 4)
            th2 = 4;
        int rng = vsb_h - th2;
        int ty2 = vsb_y + (ms > 0 ? (s->scroll_row * rng) / ms : 0);
        fill_rect(vsb_x + 1, ty2, SCROLLBAR_W - 2, th2, LIGHT_GRAY);
    }

    draw_line(wx, hsb_y - 1, wx + ww - 1, hsb_y - 1, COL_SEP);
    draw_hsb(hsb_x, hsb_y, hsb_w, HEX_ROW_PX, s->hscroll_hex);

    fill_rect(wx, stat_y - 1, ww, STATUS_H + 3, COL_STATUS_BG);
    draw_line(wx, stat_y - 1, wx + ww - 1, stat_y - 1, COL_SEP);
    {
        char stat[80];
        if (s->status_timer > 0) {
            strncpy(stat, s->status, 79);
            stat[79] = '\0';
        } else if (s->data_len == 0) {
            strcpy(stat, "No file.  File > Open");
        } else {
            int sel = s->sel_byte;
            if (sel >= 0 && sel < s->data_len) {
                uint8_t b = s->data[sel];
                sprintf(stat, "Off:%04X  Hex:%02X  Dec:%3d  %s", sel, b, b,
                        (b >= 0x20 && b < 0x7F) ? "print" : "ctrl");
            } else {
                sprintf(stat, "%d bytes  |  %s", s->data_len,
                        s->filename[0] ? s->filename : "?");
            }
        }
        draw_string(wx + 3, stat_y + 1, stat, COL_STATUS_FG, 2);
    }

    if (s->fname_mode != FNAME_MODE_NONE) {
        int bw = ww - 8, bh = 40;
        int bx = wx + 4;
        int by = (win->y + MENUBAR_H + 16) + (wh / 2) - bh / 2;

        fill_rect(bx + 3, by + 3, bw, bh, BLACK);
        fill_rect(bx, by, bw, bh, LIGHT_GRAY);
        draw_rect(bx, by, bw, bh, BLACK);
        fill_rect(bx, by, bw, 11, DARK_GRAY);
        draw_string(bx + 4, by + 3, "Open File", WHITE, 2);
        draw_string(bx + 4, by + 15, "Path:", DARK_GRAY, 2);

        int fldx = bx + 32, fldw = bw - 36;
        fill_rect(fldx, by + 14, fldw, 10, WHITE);
        draw_rect(fldx, by + 14, fldw, 10, BLACK);

        int mc = (fldw - 4) / CHAR_W;
        if (mc < 1)
            mc = 1;
        const char *show = s->fname_buf;
        if (s->fname_len > mc)
            show = s->fname_buf + (s->fname_len - mc);
        draw_string(fldx + 2, by + 15, (char *)show, BLACK, 2);

        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 500) % 2 == 0) {
            int vis = s->fname_len < mc ? s->fname_len : mc;
            int cx = fldx + 2 + vis * CHAR_W;
            if (cx <= fldx + fldw - 2)
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

/* ── draw ASCII window ────────────────────────────────────────────────── */
static void hv_draw_asc(window *win, void *ud) {
    hexview_state_t *s = (hexview_state_t *)ud;
    if (!s)
        return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(wx, wy, ww, wh, COL_BG);

    int clip_x, clip_w, hdr_y, data_y, data_h;
    int hsb_x, hsb_y, hsb_w, stat_y;
    asc_layout(s, &clip_x, &clip_w, &hdr_y, &data_y, &data_h, &hsb_x, &hsb_y,
               &hsb_w, &stat_y);

    int max_hscroll_asc = ASC_ROW_PX - clip_w;
    if (max_hscroll_asc < 0)
        max_hscroll_asc = 0;
    if (s->hscroll_asc > max_hscroll_asc)
        s->hscroll_asc = max_hscroll_asc;
    if (s->hscroll_asc < 0)
        s->hscroll_asc = 0;

    fill_rect(wx, hdr_y - 1, ww, LINE_H + 2, COL_HEADER);
    {
        int hx = clip_x - s->hscroll_asc;
        draw_string(hx, hdr_y + 1, "ASCII", COL_HEADER_TEXT, 2);
        int mid_hx = clip_x - s->hscroll_asc + 8 * ASC_CHAR_STEP - 1;
        if (mid_hx >= clip_x && mid_hx < clip_x + clip_w)
            draw_string(mid_hx, hdr_y + 1, "|", COL_SEP, 2);
    }
    draw_line(wx, data_y - 1, wx + ww - 1, data_y - 1, COL_SEP);

    int rows = total_rows(s);
    int vrows = s->visible_rows > 0 ? s->visible_rows : 1;

    for (int row = 0; row < vrows; row++) {
        int logical = s->scroll_row + row;
        if (logical >= rows)
            break;

        int py = data_y + row * LINE_H;
        int base = logical * BYTES_PER_ROW;
        int cnt = s->data_len - base;
        if (cnt > BYTES_PER_ROW)
            cnt = BYTES_PER_ROW;

        for (int b = 0; b < cnt; b++) {
            int rx = clip_x - s->hscroll_asc + b * ASC_CHAR_STEP;

            if (rx + CHAR_W <= clip_x || rx >= clip_x + clip_w)
                continue;

            int bi = base + b;
            uint8_t byte = s->data[bi];
            bool sel = (bi == s->sel_byte);

            if (sel) {
                int fl = rx - 1 < clip_x ? clip_x : rx - 1;
                int fr = rx + CHAR_W + 1 > clip_x + clip_w ? clip_x + clip_w
                                                           : rx + CHAR_W + 1;
                fill_rect(fl, py - 1, fr - fl, LINE_H + 1, COL_ASCII_SEL_BG);
                char c = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
                draw_char(rx, py, c, COL_ASCII_SEL_FG, 2);
            } else {
                char c;
                uint8_t fg;
                if (byte >= 0x20 && byte < 0x7F) {
                    c = (char)byte;
                    fg = COL_ASCII_PRINT;
                } else if (byte == 0x00) {
                    c = '0';
                    fg = COL_HEX_ZERO;
                } else {
                    c = '.';
                    fg = COL_ASCII_CTRL;
                }
                draw_char(rx, py, c, fg, 2);
            }

            if (b == 7) {
                int sep_x = clip_x - s->hscroll_asc + 8 * ASC_CHAR_STEP - 1;
                if (sep_x >= clip_x && sep_x < clip_x + clip_w)
                    draw_line(sep_x, py, sep_x, py + LINE_H - 2, DARK_GRAY);
            }
        }
    }

    if (clip_x > wx)
        fill_rect(wx, data_y - 2, clip_x - wx, data_h + 2, COL_BG);
    int right_start = clip_x + clip_w;
    if (right_start < wx + ww)
        fill_rect(right_start, data_y - 2, wx + ww - right_start, data_h + 2,
                  COL_BG);

    draw_line(wx, hsb_y - 1, wx + ww - 1, hsb_y - 1, COL_SEP);
    draw_hsb(hsb_x, hsb_y, hsb_w, ASC_ROW_PX, s->hscroll_asc);

    fill_rect(wx, stat_y - 1, ww, STATUS_H + 3, COL_STATUS_BG);
    draw_line(wx, stat_y - 1, wx + ww - 1, stat_y - 1, COL_SEP);
    {
        char info[32];
        if (s->data_len > 0)
            sprintf(info, "%d bytes", s->data_len);
        else
            strcpy(info, "empty");
        draw_string(wx + 3, stat_y + 1, info, COL_STATUS_FG, 2);
    }
}

static hexview_state_t *active_hv(void) {
    window *fw = wm_focused_window();
    if (!fw)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &hexview_app)
            continue;
        hexview_state_t *s = (hexview_state_t *)a->state;
        if (s->win_hex == fw || s->win_asc == fw)
            return s;
    }
    return NULL;
}

static void commit_fname(hexview_state_t *s) {
    s->fname_mode = FNAME_MODE_NONE;
    const char *sep = strrchr(s->fname_buf, '/');
    const char *base = sep ? sep + 1 : s->fname_buf;
    strncpy(s->filepath, s->fname_buf, FNAME_BUF_CAP - 1);
    s->filepath[FNAME_BUF_CAP - 1] = '\0';
    strncpy(s->filename, base, FNAME_BUF_CAP - 1);
    s->filename[FNAME_BUF_CAP - 1] = '\0';
    s->win_hex->title = s->filename[0] ? s->filename : "HexView";
    s->win_asc->title = "ASCII";
    if (hv_load(s, s->filepath)) {
        char msg[64];
        sprintf(msg, "Loaded %d bytes.", s->data_len);
        hv_set_status(s, msg);
    } else {
        s->filepath[0] = '\0';
        s->filename[0] = '\0';
        s->win_hex->title = "HexView";
        hv_set_status(s, "File not found.");
    }
}

/* ── menus ────────────────────────────────────────────────────────────── */
static void menu_open(void) {
    hexview_state_t *s = active_hv();
    if (!s)
        return;
    s->fname_buf[0] = '\0';
    s->fname_len = 0;
    s->fname_mode = FNAME_MODE_OPEN;
    wm_focus(s->win_hex);
}
static void menu_close_hv(void) { os_quit_app_by_desc(&hexview_app); }
static void on_about_hv(void) {
    modal_show(MODAL_INFO, "About HexView",
               "HexView v1.2\nASMOS Hex Viewer\n"
               "Left: address + hex  Right: ASCII\n"
               "Scroll bar or arrow keys to navigate.\n"
               "Resize window to see more columns.",
               NULL, NULL);
}
static bool hv_close_cb(window *w) {
    (void)w;
    os_quit_app_by_desc(&hexview_app);
    return true;
}

/* ── init ─────────────────────────────────────────────────────────────── */
static void hexview_init(void *state) {
    hexview_state_t *s = (hexview_state_t *)state;

    {
        const window_spec_t spec = {
            .x = HEXVIEW_HEX_X,
            .y = HEXVIEW_HEX_Y,
            .w = HEXVIEW_HEX_W,
            .h = HEXVIEW_HEX_H,
            .min_w = 100,
            .min_h = 80,
            .resizable = true,
            .title = "HexView",
            .title_color = WHITE,
            .bar_color = DARK_GRAY,
            .content_color = BLACK,
            .visible = true,
            .on_close = hv_close_cb,
        };
        s->win_hex = wm_register(&spec);
        if (!s->win_hex)
            return;
        s->win_hex->on_draw = hv_draw_hex;
        s->win_hex->on_draw_userdata = s;
        menu *fm = window_add_menu(s->win_hex, "File");
        menu_add_item(fm, "Open", menu_open);
        menu_add_separator(fm);
        menu_add_item(fm, "Close", menu_close_hv);
        menu_add_item(fm, "About", on_about_hv);
    }

    {
        const window_spec_t spec = {
            .x = HEXVIEW_ASC_X,
            .y = HEXVIEW_ASC_Y,
            .w = HEXVIEW_ASC_W,
            .h = HEXVIEW_ASC_H,
            .min_w = 60,
            .min_h = 80,
            .resizable = true,
            .title = "ASCII",
            .title_color = LIGHT_GREEN,
            .bar_color = DARK_GRAY,
            .content_color = BLACK,
            .visible = true,
            .on_close = hv_close_cb,
        };
        s->win_asc = wm_register(&spec);
        if (!s->win_asc)
            return;
        s->win_asc->on_draw = hv_draw_asc;
        s->win_asc->on_draw_userdata = s;
        menu *fm = window_add_menu(s->win_asc, "File");
        menu_add_item(fm, "Open", menu_open);
        menu_add_separator(fm);
        menu_add_item(fm, "Close", menu_close_hv);
        menu_add_item(fm, "About", on_about_hv);
    }

    s->data = NULL;
    s->data_len = 0;
    s->scroll_row = 0;
    s->visible_rows = 1;
    s->hscroll_hex = 0;
    s->hscroll_asc = 0;
    s->sel_byte = -1;
    s->fname_mode = FNAME_MODE_NONE;
}

/* ── on_frame ─────────────────────────────────────────────────────────── */
static void hexview_on_frame(void *state) {
    hexview_state_t *s = (hexview_state_t *)state;
    if (!s->win_hex || !s->win_asc)
        return;
    if (!s->win_hex->visible && !s->win_asc->visible)
        return;

    bool focused = window_is_focused(s->win_hex) || window_is_focused(s->win_asc);

    if (s->status_timer > 0)
        s->status_timer--;

    if (s->fname_mode != FNAME_MODE_NONE) {
        if (kb.key_pressed && focused) {
            if (kb.ctrl && kb.key_pressed && kb.last_scancode == V_KEY) {
                if (clipboard_has_text()) {
                    for (int i = 0; i < g_clipboard.text_len &&
                                    s->fname_len < FNAME_BUF_CAP - 1;
                         i++) {
                        s->fname_buf[s->fname_len++] = g_clipboard.text[i];
                    }
                    s->fname_buf[s->fname_len] = '\0';
                }
            }
            if (kb.last_scancode == ESC) {
                s->fname_mode = FNAME_MODE_NONE;
            } else if (kb.last_scancode == ENTER && s->fname_len > 0) {
                commit_fname(s);
                hv_draw_hex(s->win_hex, s);
                hv_draw_asc(s->win_asc, s);
            } else if (kb.last_scancode == BACKSPACE) {
                if (s->fname_len > 0)
                    s->fname_buf[--s->fname_len] = '\0';
            } else if (kb.last_char >= 32 && kb.last_char < 127 &&
                       s->fname_len < FNAME_BUF_CAP - 1) {
                char c = kb.last_char;
                bool ok = (c == '/' || c == '.' || (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '_' || c == '-' || c == '!' || c == '#' ||
                           c == '$' || c == '%' || c == '&' || c == '\'' ||
                           c == '(' || c == ')' || c == '@' || c == '^' ||
                           c == '`' || c == '{' || c == '}' || c == '~');
                if (ok) {
                    s->fname_buf[s->fname_len++] = c;
                    s->fname_buf[s->fname_len] = '\0';
                }
            }
        }
        if (mouse.left_clicked && focused) {
            window *hw = s->win_hex;
            int wx2 = hw->x + 1, wy2 = hw->y + MENUBAR_H + 16;
            int ww2 = hw->w - 2, wh2 = hw->h - 16;
            int bw = ww2 - 8, bh = 40, bx = wx2 + 4,
                by = wy2 + wh2 / 2 - bh / 2;
            bool ok_h = (mouse.x >= bx + 4 && mouse.x < bx + 34 &&
                         mouse.y >= by + 28 && mouse.y < by + 36);
            bool can_h = (mouse.x >= bx + 38 && mouse.x < bx + 78 &&
                          mouse.y >= by + 28 && mouse.y < by + 36);
            if (can_h)
                s->fname_mode = FNAME_MODE_NONE;
            else if (ok_h && s->fname_len > 0) {
                commit_fname(s);
                hv_draw_hex(s->win_hex, s);
                hv_draw_asc(s->win_asc, s);
            }
        }
        hv_draw_hex(s->win_hex, s);
        hv_draw_asc(s->win_asc, s);
        return;
    }

    if (focused) {
        int clip_x, clip_w, hdr_y, data_y, data_h;
        int vsb_x, vsb_y, vsb_h, hsb_x, hsb_y, hsb_w, stat_y;
        hex_layout(s, &clip_x, &clip_w, &hdr_y, &data_y, &data_h, &vsb_x,
                   &vsb_y, &vsb_h, &hsb_x, &hsb_y, &hsb_w, &stat_y);

        int rows = total_rows(s);
        int max_hscroll_hex = HEX_ROW_PX - clip_w;
        if (max_hscroll_hex < 0)
            max_hscroll_hex = 0;

        if ((mouse.left_clicked || mouse.left) && mouse.x >= vsb_x &&
            mouse.x < vsb_x + SCROLLBAR_W && mouse.y >= vsb_y &&
            mouse.y < vsb_y + vsb_h && rows > s->visible_rows) {
            int ms = rows - s->visible_rows;
            int ns = ((mouse.y - vsb_y) * ms) / vsb_h;
            if (ns < 0)
                ns = 0;
            if (ns > ms)
                ns = ms;
            s->scroll_row = ns;
        }

        if ((mouse.left_clicked || mouse.left) && mouse.y >= hsb_y &&
            mouse.y < hsb_y + SCROLLBAR_H && mouse.x >= hsb_x &&
            mouse.x < hsb_x + hsb_w && max_hscroll_hex > 0) {
            s->hscroll_hex = hsb_pick(mouse.x, hsb_x, hsb_w, HEX_ROW_PX);
        }

        if (mouse.left_clicked && s->data_len > 0 && mouse.x >= clip_x &&
            mouse.x < clip_x + clip_w && mouse.y >= data_y &&
            mouse.y < data_y + data_h) {
            int row = (mouse.y - data_y) / LINE_H;
            int logical = s->scroll_row + row;
            if (logical < rows) {
                int base = logical * BYTES_PER_ROW;
                int vx = mouse.x - clip_x + s->hscroll_hex;
                int hex_start = 5 * CHAR_W;
                int rel = vx - hex_start;
                if (rel >= 0) {
                    int col = rel / (3 * CHAR_W);
                    if (col >= 0 && col < BYTES_PER_ROW) {
                        int idx = base + col;
                        if (idx < s->data_len)
                            s->sel_byte = idx;
                    }
                }
            }
        }

        asc_layout(s, &clip_x, &clip_w, &hdr_y, &data_y, &data_h, &hsb_x,
                   &hsb_y, &hsb_w, &stat_y);

        int max_hscroll_asc = ASC_ROW_PX - clip_w;
        if (max_hscroll_asc < 0)
            max_hscroll_asc = 0;

        if ((mouse.left_clicked || mouse.left) && mouse.y >= hsb_y &&
            mouse.y < hsb_y + SCROLLBAR_H && mouse.x >= hsb_x &&
            mouse.x < hsb_x + hsb_w && max_hscroll_asc > 0) {
            s->hscroll_asc = hsb_pick(mouse.x, hsb_x, hsb_w, ASC_ROW_PX);
        }

        if (mouse.left_clicked && s->data_len > 0 && mouse.x >= clip_x &&
            mouse.x < clip_x + clip_w && mouse.y >= data_y &&
            mouse.y < data_y + data_h) {
            int row = (mouse.y - data_y) / LINE_H;
            int logical = s->scroll_row + row;
            if (logical < total_rows(s)) {
                int base = logical * BYTES_PER_ROW;
                int vx = mouse.x - clip_x + s->hscroll_asc;
                int col = vx / ASC_CHAR_STEP;
                if (col >= 0 && col < BYTES_PER_ROW) {
                    int idx = base + col;
                    if (idx < s->data_len)
                        s->sel_byte = idx;
                }
            }
        }

        window *fw = wm_focused_window();
        bool our_win = (fw == s->win_hex || fw == s->win_asc);
        if (our_win && kb.key_pressed && s->data_len > 0) {
            uint8_t sc = kb.last_scancode;
            int sel = s->sel_byte < 0 ? 0 : s->sel_byte;

            if (sc == RIGHT_ARROW)
                sel++;
            else if (sc == LEFT_ARROW)
                sel--;
            else if (sc == DOWN_ARROW)
                sel += BYTES_PER_ROW;
            else if (sc == UP_ARROW)
                sel -= BYTES_PER_ROW;
            else if (sc == PAGE_DOWN)
                sel += BYTES_PER_ROW * s->visible_rows;
            else if (sc == PAGE_UP)
                sel -= BYTES_PER_ROW * s->visible_rows;
            else if (sc == HOME)
                sel = (sel / BYTES_PER_ROW) * BYTES_PER_ROW;
            else if (sc == END)
                sel = (sel / BYTES_PER_ROW) * BYTES_PER_ROW + BYTES_PER_ROW - 1;
            else if (sc == F5)
                sel = 0;
            else if (sc == F6)
                sel = s->data_len - 1;

            if (sel < 0)
                sel = 0;
            if (sel >= s->data_len)
                sel = s->data_len - 1;
            s->sel_byte = sel;
            hv_scroll_to_sel(s);
        }
    }
}

/* ── destroy ──────────────────────────────────────────────────────────── */
static void hexview_destroy(void *state) {
    hexview_state_t *s = (hexview_state_t *)state;
    if (s->data) {
        kfree(s->data);
        s->data = NULL;
    }
    if (s->win_hex) {
        wm_unregister(s->win_hex);
        s->win_hex = NULL;
    }
    if (s->win_asc) {
        wm_unregister(s->win_asc);
        s->win_asc = NULL;
    }
}

app_descriptor hexview_app = {
    .name = "HEXVIEW",
    .state_size = sizeof(hexview_state_t),
    .init = hexview_init,
    .on_frame = hexview_on_frame,
    .destroy = hexview_destroy,
};
