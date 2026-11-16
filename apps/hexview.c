/* HexView - read-only hex + ASCII viewer (files up to 64 KB). */
#include "os/api.h"

#define MAX_FILE_SIZE 65536
#define CW 5
#define LH 8
#define ADDR_CHARS 6                     /* "0000: " */
#define HEX_X (ADDR_CHARS * CW)          /* hex column start           */
#define ASC_X (HEX_X + BPR * 3 * CW + 6) /* ascii column start         */
#define ROW_PX (HEX_X + 16 * 3 * CW + 6 + 16 * 6 + 4) /* fits 16/row */
#define BPR (s->bpr) /* 16 bytes per row when the view is wide, else 8 */

typedef struct {
    window *win; /* must stay first */
    control *view, *vsb, *status;
    uint8_t *data;
    int len, top, sel, rows_vis, bpr;
    char path[FS_PATH_SZ];
} hexview_state_t;

app_descriptor hexview_app;

static int total_rows(const hexview_state_t *s) {
    return (s->len + BPR - 1) / BPR;
}

static void update(hexview_state_t *s) {
    int maxtop = total_rows(s) - s->rows_vis;
    if (maxtop < 0)
        maxtop = 0;
    if (s->sel >= 0) {
        int r = s->sel / BPR;
        if (r < s->top)
            s->top = r;
        if (r >= s->top + s->rows_vis)
            s->top = r - s->rows_vis + 1;
    }
    if (s->top > maxtop)
        s->top = maxtop;
    if (s->top < 0)
        s->top = 0;
    ctl_set_range(s->vsb, 0, maxtop, s->rows_vis);
    ctl_set_value(s->vsb, s->top);

    char b[96];
    const char *name = strrchr(s->path, '/');
    name = name ? name + 1 : s->path;
    if (!s->len)
        snprintf(b, sizeof(b), "%s",
                 s->path[0] ? "Empty file." : "No file. File > Open...");
    else {
        uint8_t v = s->data[s->sel];
        snprintf(b, sizeof(b), "%04X: %02X %3d '%c'\t%d bytes  %s", s->sel, v,
                 v, (v >= 32 && v < 127) ? v : '.', s->len, name);
    }
    ctl_set_text(s->status, b);
}

static bool load(hexview_state_t *s, const char *path) {
    fat_file_t f;
    if (!fs_open(path, &f))
        return false;
    uint32_t n =
        f.entry.file_size > MAX_FILE_SIZE ? MAX_FILE_SIZE : f.entry.file_size;
    uint8_t *buf = n ? (uint8_t *)kmalloc(n) : NULL;
    int got = buf ? fs_read(&f, buf, (int)n) : 0;
    fs_close(&f);
    if (s->data)
        kfree(s->data);
    s->data = buf;
    s->len = got > 0 ? got : 0;
    s->top = 0;
    s->sel = 0;
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->win->title = s->path;
    update(s);
    return true;
}

/* ── the hex view (custom control) ─────────────────────────────────────── */

static void paint(control *c, int x, int y, int w, int h) {
    hexview_state_t *s = c->user;
    static const char *HX = "0123456789ABCDEF";
    int bpr = w >= ROW_PX ? 16 : 8;
    if (bpr != s->bpr) { /* window resized across the threshold */
        s->bpr = bpr;
        update(s);
    }
    fill_rect(x, y, w, h, BLACK);
    /* header */
    fill_rect(x, y, w, LH + 1, DARK_GRAY);
    draw_string(x + 2, y + 1, "ADDR", YELLOW, 2);
    for (int b = 0; b < BPR; b++)
        draw_char(x + 2 + HEX_X + b * 3 * CW + CW, y + 1, HX[b],
                  b & 1 ? LIGHT_GRAY : WHITE, 2);
    draw_string(x + 2 + ASC_X, y + 1, "ASCII", YELLOW, 2);
    s->rows_vis = (h - LH - 2) / LH;
    if (s->rows_vis < 1)
        s->rows_vis = 1;
    for (int r = 0; r < s->rows_vis; r++) {
        int row = s->top + r;
        if (row >= total_rows(s))
            break;
        int py = y + LH + 2 + r * LH;
        int base = row * BPR;
        char addr[8];
        snprintf(addr, sizeof(addr), "%04X:", base);
        draw_string(x + 2, py, addr, CYAN, 2);
        for (int b = 0; b < BPR && base + b < s->len; b++) {
            int i = base + b;
            uint8_t v = s->data[i];
            int hx = x + 2 + HEX_X + b * 3 * CW;
            int ax = x + 2 + ASC_X + b * 6;
            bool sel = (i == s->sel);
            uint8_t col = sel       ? WHITE
                          : !v      ? DARK_GRAY
                          : (b & 1) ? LIGHT_GRAY
                                    : WHITE;
            if (sel) {
                fill_rect(hx - 1, py - 1, 2 * CW + 1, LH,
                          c->focused ? BLUE : DARK_GRAY);
                fill_rect(ax - 1, py - 1, CW + 1, LH,
                          c->focused ? RED : DARK_GRAY);
            }
            draw_char(hx, py, HX[v >> 4], col, 2);
            draw_char(hx + CW, py, HX[v & 15], col, 2);
            char ch = (v >= 32 && v < 127) ? (char)v : '.';
            draw_char(ax, py, ch,
                      sel                    ? WHITE
                      : (v >= 32 && v < 127) ? LIGHT_GREEN
                                             : DARK_GRAY,
                      2);
        }
    }
    draw_line(x + 2 + ASC_X - 4, y, x + 2 + ASC_X - 4, y + h - 1, DARK_GRAY);
}

static void on_view(control *c, int ev) {
    hexview_state_t *s = c->user;
    if (!s->len)
        return;
    int sel = s->sel;
    if (ev == EV_MOUSEDOWN || ev == EV_DBLCLICK ||
        (ev == EV_MOUSEMOVE && mouse.left)) {
        int r = (c->my - LH - 2) / LH;
        int mx = c->mx - 2;
        int b = -1;
        if (mx >= ASC_X)
            b = (mx - ASC_X) / 6;
        else if (mx >= HEX_X)
            b = (mx - HEX_X) / (3 * CW);
        if (r >= 0 && b >= 0 && b < BPR)
            sel = (s->top + r) * BPR + b;
    } else if (ev == EV_KEY) {
        int page = BPR * (s->rows_vis > 1 ? s->rows_vis - 1 : 1);
        switch (kb.last_scancode) {
        case LEFT_ARROW:
            sel--;
            break;
        case RIGHT_ARROW:
            sel++;
            break;
        case UP_ARROW:
            sel -= BPR;
            break;
        case DOWN_ARROW:
            sel += BPR;
            break;
        case PAGE_UP:
            sel -= page;
            break;
        case PAGE_DOWN:
            sel += page;
            break;
        case HOME:
            sel = kb.ctrl ? 0 : sel - sel % BPR;
            break;
        case END:
            sel = kb.ctrl ? s->len - 1 : sel - sel % BPR + BPR - 1;
            break;
        default:
            return;
        }
    } else {
        return;
    }
    if (sel >= s->len)
        sel = s->len - 1;
    if (sel < 0)
        sel = 0;
    s->sel = sel;
    update(s);
}

static void on_scroll(control *c, int ev) {
    hexview_state_t *s = c->user;
    if (ev != EV_CHANGE)
        return;
    s->top = ctl_value(c);
    int r = s->sel / BPR; /* keep the selection on screen */
    if (r < s->top)
        s->sel = s->top * BPR + s->sel % BPR;
    else if (r >= s->top + s->rows_vis)
        s->sel = (s->top + s->rows_vis - 1) * BPR + s->sel % BPR;
    if (s->sel >= s->len)
        s->sel = s->len - 1;
    update(s);
}

/* ── menus ─────────────────────────────────────────────────────────────── */

static void opened(void *user, const char *path) {
    hexview_state_t *s = user;
    if (path && !load(s, path))
        dlg_message(s->win, "HexView", "Could not open the file.");
}

static void goto_entered(void *user, const char *text) {
    hexview_state_t *s = user;
    if (!text || !s->len)
        return;
    int v = 0;
    for (int i = 0; text[i]; i++) {
        char ch = text[i] | 0x20;
        int d = (ch >= '0' && ch <= '9')   ? ch - '0'
                : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                                           : -1;
        if (d < 0)
            continue;
        v = v * 16 + d;
    }
    s->sel = v < s->len ? v : s->len - 1;
    update(s);
}

static void menu_open(void) {
    hexview_state_t *s = ui_active_state();
    if (s)
        dlg_open_file(s->win, "Open", "/", NULL, opened, s);
}
static void menu_goto(void) {
    hexview_state_t *s = ui_active_state();
    if (s && s->len)
        dlg_input(s->win, "Go to", "Offset (hex):", "0", 8, ES_UPPER,
                  goto_entered, s);
}
static void menu_close(void) {
    hexview_state_t *s = ui_active_state();
    if (s)
        os_close_own_instance(s->win);
}
static void menu_about(void) {
    dlg_message(NULL, "About HexView",
                "HexView v3.0\nHex + ASCII viewer (files up to 64 KB)\n"
                "Arrows/PgUp/PgDn move, Ctrl+Home/End jump.");
}

static void hexview_init(void *state) {
    hexview_state_t *s = state;
    /* 16 bytes per row on a 640-wide screen, 8 on 320; resizable either way */
    int w = g_screen_width >= 640 ? 396 : 236,
        h = g_screen_height >= 400 ? 180 : 150;
    s->bpr = g_screen_width >= 640 ? 16 : 8;
    s->win = app_window(s, "HexView", 10, 10, w, h, DARK_GRAY, AW_RESIZABLE);
    if (!s->win)
        return;
    s->win->lazy_custom = true; /* the view only changes in handlers */
    s->win->min_w = 236;
    s->win->min_h = 100;
    int cw = w - 2, ch = h - 17;
    s->view =
        ctl_custom(s->win, 2, 2, cw - 14, ch - 18, paint, on_view,
                   CS_BORDER | CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);
    ctl_set_colors(s->view, 0xFF, BLACK);
    s->vsb = ctl_vscroll(s->win, cw - 10, 2, ch - 18, on_scroll);
    ctl_style_add(s->vsb, CS_ANCHOR_RIGHT | CS_STRETCH_H);
    s->status = ctl_status(s->win, "");
    s->rows_vis = 18;
    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "Open...", menu_open);
    menu_add_item(f, "Go to Offset...", menu_goto);
    menu_add_separator(f);
    menu_add_item(f, "Close", menu_close);
    menu_add_item(f, "About", menu_about);
    update(s);
    ctl_focus(s->view);
}

static void hexview_destroy(void *state) {
    hexview_state_t *s = state;
    if (s->data)
        kfree(s->data);
    s->data = NULL;
}

/* Used by FileF: open a file in a new HexView window. */
void hexview_open_file(const char *path) {
    app_instance_t *a = os_launch_app(&hexview_app);
    if (a && a->state)
        opened(a->state, path);
}

app_descriptor hexview_app = {
    .name = "HEXVIEW",
    .state_size = sizeof(hexview_state_t),
    .init = hexview_init,
    .destroy = hexview_destroy,
};
