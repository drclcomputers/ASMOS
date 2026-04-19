#include "os/api.h"

#define HEXVIEW_HEX_W       220
#define HEXVIEW_HEX_H       170
#define HEXVIEW_HEX_X       15
#define HEXVIEW_HEX_Y       15

#define HEXVIEW_ASC_W       110
#define HEXVIEW_ASC_H       170
#define HEXVIEW_ASC_X       (HEXVIEW_HEX_X + HEXVIEW_HEX_W + 4)
#define HEXVIEW_ASC_Y       HEXVIEW_HEX_Y

#define BYTES_PER_ROW       16
#define MAX_FILE_SIZE       65536

#define CHAR_W              5
#define CHAR_H              6
#define LINE_H              8
#define MARGIN_X            4
#define MARGIN_Y            4
#define SCROLLBAR_W         6
#define STATUS_H            8

#define COL_BG              BLACK
#define COL_ADDR            CYAN
#define COL_HEX_EVEN        WHITE
#define COL_HEX_ODD         LIGHT_GRAY
#define COL_HEX_ZERO        DARK_GRAY
#define COL_SEL_BG          LIGHT_BLUE
#define COL_SEL_FG          WHITE
#define COL_HEADER          DARK_GRAY
#define COL_HEADER_TEXT     LIGHT_YELLOW
#define COL_SEP             DARK_GRAY
#define COL_STATUS_BG       DARK_GRAY
#define COL_STATUS_FG       LIGHT_CYAN
#define COL_ASCII_PRINT     LIGHT_GREEN
#define COL_ASCII_CTRL      DARK_GRAY
#define COL_ASCII_SEL_BG    RED
#define COL_ASCII_SEL_FG    WHITE

#define FNAME_BUF_CAP       256
#define FNAME_MODE_NONE     0
#define FNAME_MODE_OPEN     1

typedef struct {
    window  *win_hex;
    window  *win_asc;

    uint8_t *data;
    int      data_len;

    int      scroll_row;
    int      visible_rows;

    int      sel_byte;

    char     filepath[FNAME_BUF_CAP];
    char     filename[FNAME_BUF_CAP];

    int      fname_mode;
    char     fname_buf[FNAME_BUF_CAP];
    int      fname_len;

    char     status[64];
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
    if (s->data_len == 0) return 0;
    return (s->data_len + BYTES_PER_ROW - 1) / BYTES_PER_ROW;
}

static void hv_clamp_scroll(hexview_state_t *s) {
    int rows = total_rows(s);
    int max_scroll = rows - s->visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (s->scroll_row > max_scroll) s->scroll_row = max_scroll;
    if (s->scroll_row < 0) s->scroll_row = 0;
}

static void hv_scroll_to_sel(hexview_state_t *s) {
    if (s->sel_byte < 0) return;
    int row = s->sel_byte / BYTES_PER_ROW;
    if (row < s->scroll_row)
        s->scroll_row = row;
    if (row >= s->scroll_row + s->visible_rows)
        s->scroll_row = row - s->visible_rows + 1;
    hv_clamp_scroll(s);
}

static bool hv_load(hexview_state_t *s, const char *path) {
    fat16_file_t f;
    if (!fat16_open(path, &f)) return false;

    uint32_t fsize = f.entry.file_size;
    if (fsize == 0) {
        fat16_close(&f);
        if (s->data) { kfree(s->data); s->data = NULL; }
        s->data_len = 0;
        s->scroll_row = 0;
        s->sel_byte = -1;
        return true;
    }

    uint32_t cap = fsize > MAX_FILE_SIZE ? MAX_FILE_SIZE : fsize;
    uint8_t *buf = (uint8_t *)kmalloc(cap);
    if (!buf) { fat16_close(&f); return false; }

    int got = fat16_read(&f, buf, (int)cap);
    fat16_close(&f);
    if (got < 0) got = 0;

    if (s->data) kfree(s->data);
    s->data = buf;
    s->data_len = got;
    s->scroll_row = 0;
    s->sel_byte = got > 0 ? 0 : -1;
    return true;
}

/* ── hex window layout ────────────────────────────────────────────────── */
static void hex_layout(const hexview_state_t *s,
                        int *tx, int *ty, int *tw, int *th,
                        int *sbx, int *sby, int *sbh,
                        int *stat_y, int *hdr_y)
{
    window *w = s->win_hex;
    int wx = w->x + 1;
    int wy = w->y + MENUBAR_H + 16;
    int ww = w->w - 2;
    int wh = w->h - 16;

    *hdr_y  = wy + MARGIN_Y;
    *stat_y = wy + wh - STATUS_H - 2;
    int hdr = LINE_H + 2;
    int bot = STATUS_H + 3;

    *tx = wx + MARGIN_X;
    *ty = wy + MARGIN_Y + hdr;
    *tw = ww - MARGIN_X * 2 - SCROLLBAR_W - 2;
    *th = wh - MARGIN_Y * 2 - bot - hdr;

    *sbx = wx + ww - SCROLLBAR_W - 1;
    *sby = *ty;
    *sbh = *th;
}

/* ── ASCII window layout ──────────────────────────────────────────────── */
static void asc_layout(const hexview_state_t *s,
                        int *tx, int *ty, int *tw, int *th,
                        int *hdr_y)
{
    window *w = s->win_asc;
    int wx = w->x + 1;
    int wy = w->y + MENUBAR_H + 16;
    int ww = w->w - 2;
    int wh = w->h - 16;

    *hdr_y = wy + MARGIN_Y;
    int hdr = LINE_H + 2;

    *tx = wx + MARGIN_X;
    *ty = wy + MARGIN_Y + hdr;
    *tw = ww - MARGIN_X * 2;
    *th = wh - MARGIN_Y * 2 - hdr - STATUS_H - 3;
}

/* ── draw hex window ──────────────────────────────────────────────────── */
static void draw_hex_nibble(int x, int y, uint8_t nibble, uint8_t col) {
    char c = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
    draw_char(x, y, c, col, 2);
}

static void hv_draw_hex(window *win, void *ud) {
    hexview_state_t *s = (hexview_state_t *)ud;
    if (!s) return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(wx, wy, ww, wh, COL_BG);

    int tx, ty, tw, th, sbx, sby, sbh, stat_y, hdr_y;
    hex_layout(s, &tx, &ty, &tw, &th, &sbx, &sby, &sbh, &stat_y, &hdr_y);

    int vrows = th / LINE_H;
    if (vrows < 1) vrows = 1;
    s->visible_rows = vrows;

    fill_rect(wx, hdr_y - 1, ww - SCROLLBAR_W - 2, LINE_H + 2, COL_HEADER);
    {
        int hx = tx;
        draw_string(hx, hdr_y + 1, "ADDR ", COL_HEADER_TEXT, 2);
        hx += 5 * CHAR_W;
        for (int b = 0; b < BYTES_PER_ROW; b++) {
            char tmp[3];
            tmp[0] = '0';
            tmp[1] = "0123456789ABCDEF"[b];
            tmp[2] = '\0';
            uint8_t hcol = (b % 2 == 0) ? COL_HEX_EVEN : COL_HEX_ODD;
            draw_string(hx, hdr_y + 1, tmp, hcol, 2);
            hx += 3 * CHAR_W;
        }
    }
    draw_line(wx, ty - 1, wx + ww - SCROLLBAR_W - 2, ty - 1, COL_SEP);

    int rows = total_rows(s);
    for (int row = 0; row < vrows; row++) {
        int logical = s->scroll_row + row;
        if (logical >= rows) break;

        int py   = ty + row * LINE_H;
        int base = logical * BYTES_PER_ROW;
        int cnt  = s->data_len - base;
        if (cnt > BYTES_PER_ROW) cnt = BYTES_PER_ROW;

        int rx = tx;

        {
            uint16_t a = (uint16_t)base;
            char addr[6];
            addr[0] = "0123456789ABCDEF"[(a >> 12) & 0xF];
            addr[1] = "0123456789ABCDEF"[(a >>  8) & 0xF];
            addr[2] = "0123456789ABCDEF"[(a >>  4) & 0xF];
            addr[3] = "0123456789ABCDEF"[ a        & 0xF];
            addr[4] = ' '; addr[5] = '\0';
            draw_string(rx, py, addr, COL_ADDR, 2);
            rx += 5 * CHAR_W;
        }

        for (int b = 0; b < BYTES_PER_ROW; b++) {
            if (b < cnt) {
                int bi       = base + b;
                uint8_t byte = s->data[bi];
                bool sel     = (bi == s->sel_byte);

                if (sel)
                    fill_rect(rx - 1, py - 1, 2 * CHAR_W + 2, LINE_H + 1, COL_SEL_BG);

                uint8_t col;
                if      (sel)       col = COL_SEL_FG;
                else if (!byte)     col = COL_HEX_ZERO;
                else if (b % 2==0)  col = COL_HEX_EVEN;
                else                col = COL_HEX_ODD;

                draw_hex_nibble(rx,          py, (byte >> 4) & 0xF, col);
                draw_hex_nibble(rx + CHAR_W, py,  byte       & 0xF, col);
            }
            rx += 3 * CHAR_W;
        }
    }

    fill_rect(sbx, sby, SCROLLBAR_W, sbh, COL_HEADER);
    draw_rect(sbx, sby, SCROLLBAR_W, sbh, COL_SEP);
    if (rows > vrows) {
        int max_sc = rows - vrows;
        int thumb  = (vrows * sbh) / rows;
        if (thumb < 4) thumb = 4;
        int range  = sbh - thumb;
        int ty2    = sby + (max_sc > 0 ? (s->scroll_row * range) / max_sc : 0);
        fill_rect(sbx + 1, ty2, SCROLLBAR_W - 2, thumb, LIGHT_GRAY);
    }

    fill_rect(wx, stat_y - 1, ww, STATUS_H + 3, COL_STATUS_BG);
    draw_line(wx, stat_y - 1, wx + ww - 1, stat_y - 1, COL_SEP);
    {
        char stat[80];
        if (s->status_timer > 0) {
            strncpy(stat, s->status, 79); stat[79] = '\0';
        } else if (s->data_len == 0) {
            strcpy(stat, "No file.  File > Open");
        } else {
            int sel = s->sel_byte;
            if (sel >= 0 && sel < s->data_len) {
                uint8_t b = s->data[sel];
                sprintf(stat, "Off:%04X  Hex:%02X  Dec:%3d  %s",
                        sel, b, b,
                        (b >= 0x20 && b < 0x7F) ? "print" : "ctrl");
            } else {
                sprintf(stat, "%d bytes  |  %s",
                        s->data_len,
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
        if (mc < 1) mc = 1;
        const char *show = s->fname_buf;
        if (s->fname_len > mc) show = s->fname_buf + (s->fname_len - mc);
        draw_string(fldx + 2, by + 15, (char *)show, BLACK, 2);

        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 50) % 2 == 0) {
            int vis = s->fname_len < mc ? s->fname_len : mc;
            int cx  = fldx + 2 + vis * CHAR_W;
            if (cx <= fldx + fldw - 2)
                draw_string(cx, by + 15, "|", BLACK, 2);
        }

        fill_rect(bx + 4,  by + 28, 30, 8, LIGHT_BLUE);
        draw_rect(bx + 4,  by + 28, 30, 8, BLACK);
        draw_string(bx + 11, by + 29, "OK", WHITE, 2);

        fill_rect(bx + 38, by + 28, 40, 8, DARK_GRAY);
        draw_rect(bx + 38, by + 28, 40, 8, BLACK);
        draw_string(bx + 41, by + 29, "Cancel", WHITE, 2);
    }
}

/* ── draw ASCII window ────────────────────────────────────────────────── */
static void hv_draw_asc(window *win, void *ud) {
    hexview_state_t *s = (hexview_state_t *)ud;
    if (!s) return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(wx, wy, ww, wh, COL_BG);

    int tx, ty, tw, th, hdr_y;
    asc_layout(s, &tx, &ty, &tw, &th, &hdr_y);

    fill_rect(wx, hdr_y - 1, ww, LINE_H + 2, COL_HEADER);
    draw_string(tx, hdr_y + 1, "ASCII", COL_HEADER_TEXT, 2);

    int mid_lbl_x = tx + 8 * (CHAR_W + 1) - 1;
    draw_string(mid_lbl_x, hdr_y + 1, "|", COL_SEP, 2);
    draw_line(wx, ty - 1, wx + ww - 1, ty - 1, COL_SEP);

    int rows  = total_rows(s);
    int vrows = s->visible_rows > 0 ? s->visible_rows : 1;

    for (int row = 0; row < vrows; row++) {
        int logical = s->scroll_row + row;
        if (logical >= rows) break;

        int py   = ty + row * LINE_H;
        int base = logical * BYTES_PER_ROW;
        int cnt  = s->data_len - base;
        if (cnt > BYTES_PER_ROW) cnt = BYTES_PER_ROW;

        for (int b = 0; b < cnt; b++) {
            int bi       = base + b;
            uint8_t byte = s->data[bi];
            bool sel     = (bi == s->sel_byte);

            int rx = tx + b * (CHAR_W + 1);

            if (sel) {
                fill_rect(rx - 1, py - 1, CHAR_W + 2, LINE_H + 1, COL_ASCII_SEL_BG);
                char c = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
                draw_char(rx, py, c, COL_ASCII_SEL_FG, 2);
            } else {
                char c;
                uint8_t fg;
                if (byte >= 0x20 && byte < 0x7F) {
                    c  = (char)byte; fg = COL_ASCII_PRINT;
                } else if (byte == 0x00) {
                    c  = '0';       fg = COL_HEX_ZERO;
                } else {
                    c  = '.';       fg = COL_ASCII_CTRL;
                }
                draw_char(rx, py, c, fg, 2);
            }

            if (b == 7) {
                int sep_x = tx + 8 * (CHAR_W + 1) - 1;
                draw_line(sep_x, py, sep_x, py + LINE_H - 2, DARK_GRAY);
            }
        }
    }

    {
        int by2 = wy + wh - STATUS_H - 2;
        fill_rect(wx, by2 - 1, ww, STATUS_H + 3, COL_STATUS_BG);
        draw_line(wx, by2 - 1, wx + ww - 1, by2 - 1, COL_SEP);
        char info[32];
        if (s->data_len > 0)
            sprintf(info, "%d bytes", s->data_len);
        else
            strcpy(info, "empty");
        draw_string(wx + 3, by2 + 1, info, COL_STATUS_FG, 2);
    }
}

/* ── active instance ──────────────────────────────────────────────────── */
static hexview_state_t *active_hv(void) {
    window *fw = wm_focused_window();
    if (!fw) return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &hexview_app) continue;
        hexview_state_t *s = (hexview_state_t *)a->state;
        if (s->win_hex == fw || s->win_asc == fw) return s;
    }
    return NULL;
}

/* ── commit file path ─────────────────────────────────────────────────── */
static void commit_fname(hexview_state_t *s) {
    s->fname_mode = FNAME_MODE_NONE;
    const char *sep  = strrchr(s->fname_buf, '/');
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
    if (!s) return;
    s->fname_buf[0] = '\0';
    s->fname_len = 0;
    s->fname_mode = FNAME_MODE_OPEN;
    wm_focus(s->win_hex);
}

static void menu_close_hv(void) { os_quit_app_by_desc(&hexview_app); }

static void on_about_hv(void) {
    modal_show(MODAL_INFO, "About HexView",
               "HexView v1.1\nASMOS Hex Viewer\n"
               "Left: addresses + hex bytes\n"
               "Right: ASCII representation\n"
               "Click either panel to select a byte.",
               NULL, NULL);
}

/* ── close callback shared by both windows ────────────────────────────── */
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
            .min_w = 180,
            .min_h = 80,
            .resizable     = true,
            .title         = "HexView",
            .title_color   = WHITE,
            .bar_color     = DARK_GRAY,
            .content_color = BLACK,
            .visible       = true,
            .on_close      = hv_close_cb,
        };
        s->win_hex = wm_register(&spec);
        if (!s->win_hex) return;
        s->win_hex->on_draw          = hv_draw_hex;
        s->win_hex->on_draw_userdata = s;

        menu *fm = window_add_menu(s->win_hex, "File");
        menu_add_item(fm, "Open",  menu_open);
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
            .min_w = 80,
            .min_h = 80,
            .resizable     = true,
            .title         = "ASCII",
            .title_color   = LIGHT_GREEN,
            .bar_color     = DARK_GRAY,
            .content_color = BLACK,
            .visible       = true,
            .on_close      = hv_close_cb,
        };
        s->win_asc = wm_register(&spec);
        if (!s->win_asc) return;
        s->win_asc->on_draw          = hv_draw_asc;
        s->win_asc->on_draw_userdata = s;

        menu *fm = window_add_menu(s->win_asc, "File");
        menu_add_item(fm, "Open",  menu_open);
        menu_add_separator(fm);
        menu_add_item(fm, "Close", menu_close_hv);
        menu_add_item(fm, "About", on_about_hv);
    }

    s->data         = NULL;
    s->data_len     = 0;
    s->scroll_row   = 0;
    s->visible_rows = 1;
    s->sel_byte     = -1;
    s->fname_mode   = FNAME_MODE_NONE;
}

/* ── on_frame ─────────────────────────────────────────────────────────── */
static void hexview_on_frame(void *state) {
    hexview_state_t *s = (hexview_state_t *)state;
    if (!s->win_hex || !s->win_asc) return;
    if (!s->win_hex->visible && !s->win_asc->visible) return;

    if (s->status_timer > 0) s->status_timer--;

    if (s->fname_mode != FNAME_MODE_NONE) {
        if (kb.key_pressed) {
            if (kb.last_scancode == ESC) {
                s->fname_mode = FNAME_MODE_NONE;
            } else if (kb.last_scancode == ENTER && s->fname_len > 0) {
                commit_fname(s);
                goto redraw;
            } else if (kb.last_scancode == BACKSPACE) {
                if (s->fname_len > 0)
                    s->fname_buf[--s->fname_len] = '\0';
            } else if (kb.last_char >= 32 && kb.last_char < 127 &&
                       s->fname_len < FNAME_BUF_CAP - 1) {
                char c = kb.last_char;
                bool ok = (c=='/' || c=='.' ||
                           (c>='A'&&c<='Z') || (c>='a'&&c<='z') ||
                           (c>='0'&&c<='9') ||
                           c=='_'||c=='-'||c=='!'||c=='#'||c=='$'||
                           c=='%'||c=='&'||c=='\''||c=='('||c==')'||
                           c=='@'||c=='^'||c=='`'||c=='{'||c=='}'||c=='~');
                if (ok) {
                    s->fname_buf[s->fname_len++] = c;
                    s->fname_buf[s->fname_len]   = '\0';
                }
            }
        }
        if (mouse.left_clicked) {
            window *hw  = s->win_hex;
            int wx2 = hw->x + 1;
            int wy2 = hw->y + MENUBAR_H + 16;
            int ww2 = hw->w - 2;
            int wh2 = hw->h - 16;
            int bw  = ww2 - 8, bh = 40;
            int bx  = wx2 + 4;
            int by  = wy2 + wh2 / 2 - bh / 2;

            bool ok_h  = (mouse.x >= bx+4  && mouse.x < bx+34 &&
                          mouse.y >= by+28  && mouse.y < by+36);
            bool can_h = (mouse.x >= bx+38 && mouse.x < bx+78 &&
                          mouse.y >= by+28  && mouse.y < by+36);
            if (can_h) {
                s->fname_mode = FNAME_MODE_NONE;
            } else if (ok_h && s->fname_len > 0) {
                commit_fname(s);
                goto redraw;
            }
        }
        goto redraw;
    }

    {
        int tx, ty, tw, th, sbx, sby, sbh, stat_y, hdr_y;
        hex_layout(s, &tx, &ty, &tw, &th, &sbx, &sby, &sbh, &stat_y, &hdr_y);

        int rows = total_rows(s);
        if ((mouse.left_clicked || mouse.left) &&
            mouse.x >= sbx && mouse.x < sbx + SCROLLBAR_W &&
            mouse.y >= sby && mouse.y < sby + sbh &&
            rows > s->visible_rows)
        {
            int max_sc = rows - s->visible_rows;
            int ns = ((mouse.y - sby) * max_sc) / sbh;
            if (ns < 0) ns = 0;
            if (ns > max_sc) ns = max_sc;
            s->scroll_row = ns;
        }

        if (mouse.left_clicked && s->data_len > 0 &&
            mouse.x >= tx && mouse.x < tx + tw &&
            mouse.y >= ty && mouse.y < ty + th)
        {
            int row     = (mouse.y - ty) / LINE_H;
            int logical = s->scroll_row + row;
            if (logical < total_rows(s)) {
                int base      = logical * BYTES_PER_ROW;
                int hex_start = tx + 5 * CHAR_W;
                int rel       = mouse.x - hex_start;
                if (rel >= 0) {
                    int col = rel / (3 * CHAR_W);
                    if (col >= 0 && col < BYTES_PER_ROW) {
                        int idx = base + col;
                        if (idx < s->data_len) s->sel_byte = idx;
                    }
                }
            }
        }
    }

    {
        int tx, ty, tw, th, hdr_y;
        asc_layout(s, &tx, &ty, &tw, &th, &hdr_y);

        if (mouse.left_clicked && s->data_len > 0 &&
            mouse.x >= tx && mouse.x < tx + tw &&
            mouse.y >= ty && mouse.y < ty + th)
        {
            int row     = (mouse.y - ty) / LINE_H;
            int logical = s->scroll_row + row;
            if (logical < total_rows(s)) {
                int base = logical * BYTES_PER_ROW;
                int col  = (mouse.x - tx) / (CHAR_W + 1);
                if (col >= 0 && col < BYTES_PER_ROW) {
                    int idx = base + col;
                    if (idx < s->data_len) s->sel_byte = idx;
                }
            }
        }
    }

    {
        window *fw   = wm_focused_window();
        bool our_win = (fw == s->win_hex || fw == s->win_asc);
        if (our_win && kb.key_pressed && s->data_len > 0) {
            uint8_t sc = kb.last_scancode;
            int sel    = s->sel_byte < 0 ? 0 : s->sel_byte;

            if      (sc == RIGHT_ARROW) sel++;
            else if (sc == LEFT_ARROW)  sel--;
            else if (sc == DOWN_ARROW)  sel += BYTES_PER_ROW;
            else if (sc == UP_ARROW)    sel -= BYTES_PER_ROW;
            else if (sc == PAGE_DOWN)   sel += BYTES_PER_ROW * s->visible_rows;
            else if (sc == PAGE_UP)     sel -= BYTES_PER_ROW * s->visible_rows;
            else if (sc == HOME)        sel  = (sel / BYTES_PER_ROW) * BYTES_PER_ROW;
            else if (sc == END)         sel  = (sel / BYTES_PER_ROW) * BYTES_PER_ROW + BYTES_PER_ROW - 1;
            else if (sc == F5)          sel  = 0;
            else if (sc == F6)          sel  = s->data_len - 1;

            if (sel < 0)            sel = 0;
            if (sel >= s->data_len) sel = s->data_len - 1;
            s->sel_byte = sel;
            hv_scroll_to_sel(s);
        }
    }

redraw:
    hv_draw_hex(s->win_hex, s);
    hv_draw_asc(s->win_asc, s);
}

/* ── destroy ──────────────────────────────────────────────────────────── */
static void hexview_destroy(void *state) {
    hexview_state_t *s = (hexview_state_t *)state;
    if (s->data)    { kfree(s->data); s->data = NULL; }
    if (s->win_hex) { wm_unregister(s->win_hex); s->win_hex = NULL; }
    if (s->win_asc) { wm_unregister(s->win_asc); s->win_asc = NULL; }
}

app_descriptor hexview_app = {
    .name       = "HEXVIEW",
    .state_size = sizeof(hexview_state_t),
    .init       = hexview_init,
    .on_frame   = hexview_on_frame,
    .destroy    = hexview_destroy,
};
