#include "os/api.h"

#define ASMDRAW_DEFAULT_X 20
#define ASMDRAW_DEFAULT_Y 15
#define ASMDRAW_DEFAULT_W 260
#define ASMDRAW_DEFAULT_H 162

#define TOOLBAR_W 18
#define TOOLBAR_PAD 2
#define TOOL_BTN_SZ 14

#define CANVAS_X_OFF (TOOLBAR_W + 2)
#define CANVAS_Y_OFF 0

#define PALETTE_H 10
#define PALETTE_COLS 16

#define CANVAS_MAX_W 320
#define CANVAS_MAX_H 200

#define TOOL_PENCIL 0
#define TOOL_ERASER 1
#define TOOL_LINE 2
#define TOOL_RECT 3
#define TOOL_FILL 4
#define NUM_TOOLS 5

#define BRUSH_SIZES 3

static const uint8_t PALETTE_COLORS[PALETTE_COLS] = {
    BLACK,   WHITE,         DARK_GRAY, LIGHT_GRAY, RED,    LIGHT_RED,
    GREEN,   LIGHT_GREEN,   BLUE,      LIGHT_BLUE, YELLOW, LIGHT_YELLOW,
    MAGENTA, LIGHT_MAGENTA, CYAN,      LIGHT_CYAN,
};

typedef struct {
    window *win;

    uint8_t canvas[CANVAS_MAX_H][CANVAS_MAX_W];
    int canvas_w;
    int canvas_h;

    int tool;
    int brush_size;
    uint8_t fg_color;
    uint8_t bg_color;

    bool drawing;
    int start_x, start_y;

    int last_cx, last_cy;

    int fname_mode;
    char fname_buf[64];
    int fname_len;

    char status[48];
    uint32_t status_timer;
} asmdraw_state_t;

app_descriptor asmdraw_app;

static void draw_set_status(asmdraw_state_t *s, const char *msg) {
    strncpy(s->status, msg, 47);
    s->status[47] = '\0';
    s->status_timer = 240;
}

static bool mouse_to_canvas(const asmdraw_state_t *s, int mx, int my,
                            int *cx_out, int *cy_out) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16;
    int wh = s->win->h - 16;

    int canvas_abs_x = wx + CANVAS_X_OFF;
    int canvas_abs_y = wy;
    int canvas_px_h = wh - PALETTE_H - 1;

    int cx = mx - canvas_abs_x;
    int cy = my - canvas_abs_y;

    if (cx < 0 || cy < 0 || cx >= s->canvas_w || cy >= canvas_px_h ||
        cy >= s->canvas_h)
        return false;

    *cx_out = cx;
    *cy_out = cy;
    return true;
}

static void canvas_paint(asmdraw_state_t *s, int cx, int cy, uint8_t color) {
    int r = s->brush_size;
    int lo_x = cx - r, hi_x = cx + r;
    int lo_y = cy - r, hi_y = cy + r;
    if (lo_x < 0)
        lo_x = 0;
    if (lo_y < 0)
        lo_y = 0;
    if (hi_x >= s->canvas_w)
        hi_x = s->canvas_w - 1;
    if (hi_y >= s->canvas_h)
        hi_y = s->canvas_h - 1;
    for (int y = lo_y; y <= hi_y; y++)
        for (int x = lo_x; x <= hi_x; x++)
            s->canvas[y][x] = color;
}

static void canvas_line(asmdraw_state_t *s, int x0, int y0, int x1, int y1,
                        uint8_t color) {
    int dx = x1 - x0;
    if (dx < 0)
        dx = -dx;
    int dy = y1 - y0;
    if (dy < 0)
        dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        canvas_paint(s, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void canvas_fill(asmdraw_state_t *s, int cx, int cy, uint8_t color) {
    uint8_t target = s->canvas[cy][cx];
    if (target == color)
        return;

    typedef struct {
        int16_t x, y;
    } pt_t;
    static pt_t stack[CANVAS_MAX_W * CANVAS_MAX_H / 2];
    int top = 0;

    stack[top].x = (int16_t)cx;
    stack[top].y = (int16_t)cy;
    top++;

    while (top > 0) {
        top--;
        int x = stack[top].x;
        int y = stack[top].y;
        if (x < 0 || y < 0 || x >= s->canvas_w || y >= s->canvas_h)
            continue;
        if (s->canvas[y][x] != target)
            continue;
        s->canvas[y][x] = color;
        if (top + 4 < (int)(sizeof(stack) / sizeof(stack[0]))) {
            stack[top].x = (int16_t)(x + 1);
            stack[top].y = (int16_t)y;
            top++;
            stack[top].x = (int16_t)(x - 1);
            stack[top].y = (int16_t)y;
            top++;
            stack[top].x = (int16_t)x;
            stack[top].y = (int16_t)(y + 1);
            top++;
            stack[top].x = (int16_t)x;
            stack[top].y = (int16_t)(y - 1);
            top++;
        }
    }
}

static void canvas_rect(asmdraw_state_t *s, int x0, int y0, int x1, int y1,
                        uint8_t color) {
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for (int x = x0; x <= x1; x++) {
        canvas_paint(s, x, y0, color);
        canvas_paint(s, x, y1, color);
    }
    for (int y = y0; y <= y1; y++) {
        canvas_paint(s, x0, y, color);
        canvas_paint(s, x1, y, color);
    }
}

static void update_canvas_size(asmdraw_state_t *s) {
    int ww = s->win->w - 2;
    int wh = s->win->h - 16;
    int cw = ww - CANVAS_X_OFF - 1;
    int ch = wh - PALETTE_H - 1;
    if (cw > CANVAS_MAX_W)
        cw = CANVAS_MAX_W;
    if (ch > CANVAS_MAX_H)
        ch = CANVAS_MAX_H;
    if (cw < 1)
        cw = 1;
    if (ch < 1)
        ch = 1;
    s->canvas_w = cw;
    s->canvas_h = ch;
}

static const char *TOOL_LABELS[NUM_TOOLS] = {"P", "E", "L", "R", "F"};

static void asmdraw_draw(window *win, void *ud) {
    asmdraw_state_t *s = (asmdraw_state_t *)ud;
    if (!s)
        return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(wx, wy, TOOLBAR_W, wh, DARK_GRAY);

    for (int t = 0; t < NUM_TOOLS; t++) {
        int bx = wx + TOOLBAR_PAD;
        int by = wy + TOOLBAR_PAD + t * (TOOL_BTN_SZ + TOOLBAR_PAD);
        uint8_t bg = (t == s->tool) ? LIGHT_BLUE : LIGHT_GRAY;
        fill_rect(bx, by, TOOL_BTN_SZ, TOOL_BTN_SZ, bg);
        draw_rect(bx, by, TOOL_BTN_SZ, TOOL_BTN_SZ, BLACK);
        draw_string(bx + 4, by + 4, (char *)TOOL_LABELS[t], BLACK, 2);
    }

    static const int BSIZES[BRUSH_SIZES] = {1, 3, 5};
    for (int b = 0; b < BRUSH_SIZES; b++) {
        int bx = wx + TOOLBAR_PAD;
        int by = wy + TOOLBAR_PAD +
                 (NUM_TOOLS + 1 + b) * (TOOL_BTN_SZ + TOOLBAR_PAD);
        uint8_t bg = (b == s->brush_size) ? YELLOW : LIGHT_GRAY;
        fill_rect(bx, by, TOOL_BTN_SZ, TOOL_BTN_SZ, bg);
        draw_rect(bx, by, TOOL_BTN_SZ, TOOL_BTN_SZ, BLACK);
        char lbl[3];
        lbl[0] = '0' + BSIZES[b];
        lbl[1] = '\0';
        draw_string(bx + 4, by + 4, lbl, BLACK, 2);
    }

    /*int sw_x = wx + TOOLBAR_PAD;
    int sw_y = wy + TOOLBAR_PAD + (NUM_TOOLS + BRUSH_SIZES + 2) * (TOOL_BTN_SZ +
    TOOLBAR_PAD); fill_rect(sw_x + 3, sw_y + 3, TOOL_BTN_SZ - 3, TOOL_BTN_SZ -
    3, s->bg_color); draw_rect(sw_x + 3, sw_y + 3, TOOL_BTN_SZ - 3, TOOL_BTN_SZ
    - 3, BLACK); fill_rect(sw_x,     sw_y,     TOOL_BTN_SZ - 3, TOOL_BTN_SZ - 3,
    s->fg_color); draw_rect(sw_x,     sw_y,     TOOL_BTN_SZ - 3, TOOL_BTN_SZ -
    3, BLACK);*/

    int cax = wx + CANVAS_X_OFF;
    int cay = wy;
    int cw = s->canvas_w;
    int ch = s->canvas_h;

    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            draw_dot(cax + x, cay + y, s->canvas[y][x]);

    draw_rect(cax - 1, cay - 1, cw + 2, ch + 2, DARK_GRAY);

    if (s->drawing && (s->tool == TOOL_LINE || s->tool == TOOL_RECT)) {
        int sx = cax + s->start_x;
        int sy = cay + s->start_y;
        int cx2, cy2;
        if (mouse_to_canvas(s, mouse.x, mouse.y, &cx2, &cy2)) {
            int ex = cax + cx2;
            int ey = cay + cy2;
            if (s->tool == TOOL_LINE) {
                draw_line(sx, sy, ex, ey, s->fg_color);
            } else {
                int rx0 = sx < ex ? sx : ex;
                int ry0 = sy < ey ? sy : ey;
                int rx1 = sx > ex ? sx : ex;
                int ry1 = sy > ey ? sy : ey;
                draw_rect(rx0, ry0, rx1 - rx0 + 1, ry1 - ry0 + 1, s->fg_color);
            }
        }
    }

    int pal_y = wy + wh - PALETTE_H;
    int cell_w = (ww - TOOLBAR_W - 2) / PALETTE_COLS;
    if (cell_w < 1)
        cell_w = 1;
    int pal_x0 = wx + TOOLBAR_W + 2;

    for (int i = 0; i < PALETTE_COLS; i++) {
        int px = pal_x0 + i * cell_w;
        fill_rect(px, pal_y, cell_w, PALETTE_H, PALETTE_COLORS[i]);
        draw_rect(px, pal_y, cell_w, PALETTE_H, BLACK);
    }

    if (s->status_timer > 0) {
        draw_string(wx + TOOLBAR_W + 4, pal_y - 8, s->status, DARK_GRAY, 2);
    } else {
        char info[48];
        int cx2, cy2;
        if (mouse_to_canvas(s, mouse.x, mouse.y, &cx2, &cy2)) {
            sprintf(info, "%d,%d", cx2, cy2);
        } else {
            info[0] = '\0';
        }
        draw_string(wx + TOOLBAR_W + 4, pal_y - 8, info, DARK_GRAY, 2);
    }

    if (s->fname_mode != 0) {
        int bx = wx + 20, by = wy + wh / 2 - 20;
        int bw = ww - 40, bh = 40;
        fill_rect(bx + 3, by + 3, bw, bh, BLACK);
        fill_rect(bx, by, bw, bh, LIGHT_GRAY);
        draw_rect(bx, by, bw, bh, BLACK);
        fill_rect(bx, by, bw, 11, DARK_GRAY);
        const char *title = (s->fname_mode == 1) ? "Save As" : "Open";
        draw_string(bx + 4, by + 3, (char *)title, WHITE, 2);
        draw_string(bx + 4, by + 15, "Path:", DARK_GRAY, 2);
        fill_rect(bx + 32, by + 14, bw - 36, 10, WHITE);
        draw_rect(bx + 32, by + 14, bw - 36, 10, BLACK);
        draw_string(bx + 34, by + 15, s->fname_buf, BLACK, 2);
        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 50) % 2 == 0) {
            int cx3 = bx + 34 + s->fname_len * 5;
            if (cx3 < bx + bw - 6)
                draw_string(cx3, by + 15, "|", BLACK, 2);
        }
        fill_rect(bx + 4, by + 28, 30, 8, LIGHT_BLUE);
        draw_rect(bx + 4, by + 28, 30, 8, BLACK);
        draw_string(bx + 11, by + 29, "OK", WHITE, 2);
        fill_rect(bx + 38, by + 28, 40, 8, DARK_GRAY);
        draw_rect(bx + 38, by + 28, 40, 8, BLACK);
        draw_string(bx + 41, by + 29, "Cancel", WHITE, 2);
    }
}

static void asmdraw_save(asmdraw_state_t *s, const char *path) {
    update_canvas_size(s);
    if (s->canvas_w <= 0 || s->canvas_h <= 0) {
        draw_set_status(s, "Invalid canvas size.");
        return;
    }

    dir_entry_t de;
    if (fat16_find(path, &de))
        fat16_delete(path);
    fat16_file_t f;
    if (!fat16_create(path, &f)) {
        draw_set_status(s, "Save failed.");
        return;
    }

    uint8_t hdr[4];
    hdr[0] = (uint8_t)(s->canvas_w & 0xFF);
    hdr[1] = (uint8_t)((s->canvas_w >> 8) & 0xFF);
    hdr[2] = (uint8_t)(s->canvas_h & 0xFF);
    hdr[3] = (uint8_t)((s->canvas_h >> 8) & 0xFF);
    fat16_write(&f, hdr, 4);

    for (int y = 0; y < s->canvas_h; y++)
        fat16_write(&f, s->canvas[y], s->canvas_w);

    fat16_close(&f);
    draw_set_status(s, "Saved.");
}

static void asmdraw_load(asmdraw_state_t *s, const char *path) {
    fat16_file_t f;
    if (!fat16_open(path, &f)) {
        draw_set_status(s, "File not found.");
        return;
    }

    uint8_t hdr[4];
    if (fat16_read(&f, hdr, 4) < 4) {
        fat16_close(&f);
        draw_set_status(s, "Bad file.");
        return;
    }
    int fw = hdr[0] | (hdr[1] << 8);
    int fh = hdr[2] | (hdr[3] << 8);
    if (fw < 1 || fw > CANVAS_MAX_W || fh < 1 || fh > CANVAS_MAX_H) {
        fat16_close(&f);
        draw_set_status(s, "Unsupported size.");
        return;
    }

    for (int y = 0; y < CANVAS_MAX_H; y++)
        for (int x = 0; x < CANVAS_MAX_W; x++)
            s->canvas[y][x] = WHITE;

    for (int y = 0; y < fh; y++) {
        uint8_t row[CANVAS_MAX_W];
        int got = fat16_read(&f, row, fw);
        if (got < fw)
            break;
        for (int x = 0; x < fw; x++)
            s->canvas[y][x] = row[x];
    }
    fat16_close(&f);
    draw_set_status(s, "Opened.");
}

static void commit_fname(asmdraw_state_t *s) {
    bool has_dot = false;
    for (int i = 0; i < s->fname_len; i++)
        if (s->fname_buf[i] == '.') {
            has_dot = true;
            break;
        }
    if (!has_dot && s->fname_len > 0 && s->fname_len <= 8) {
        if (s->fname_len + 4 < 64) {
            s->fname_buf[s->fname_len++] = '.';
            s->fname_buf[s->fname_len++] = 'P';
            s->fname_buf[s->fname_len++] = 'I';
            s->fname_buf[s->fname_len++] = 'C';
            s->fname_buf[s->fname_len] = '\0';
        }
    }
    int mode = s->fname_mode;
    s->fname_mode = 0;
    if (mode == 1)
        asmdraw_save(s, s->fname_buf);
    else
        asmdraw_load(s, s->fname_buf);
}

static asmdraw_state_t *active_asmdraw(void) {
    window *fw = wm_focused_window();
    if (!fw)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &asmdraw_app)
            continue;
        asmdraw_state_t *s = (asmdraw_state_t *)a->state;
        if (s->win == fw)
            return s;
    }
    return NULL;
}

static void menu_new(void) {
    asmdraw_state_t *s = active_asmdraw();
    if (!s)
        return;
    for (int y = 0; y < CANVAS_MAX_H; y++)
        for (int x = 0; x < CANVAS_MAX_W; x++)
            s->canvas[y][x] = WHITE;
    draw_set_status(s, "New canvas.");
}

static void menu_save(void) {
    asmdraw_state_t *s = active_asmdraw();
    if (!s)
        return;
    s->fname_buf[0] = '\0';
    s->fname_len = 0;
    s->fname_mode = 1;
}

static void menu_open(void) {
    asmdraw_state_t *s = active_asmdraw();
    if (!s)
        return;
    s->fname_buf[0] = '\0';
    s->fname_len = 0;
    s->fname_mode = 2;
}

static void menu_close_draw(void) {
    asmdraw_state_t *s = active_asmdraw();
    if (!s)
        return;
    os_quit_app_by_desc(&asmdraw_app);
}

static void on_about_draw(void) {
    modal_show(MODAL_INFO, "About ASMDraw", "ASMDraw v1.0\nASMOS Paint App",
               NULL, NULL);
}

static bool asmdraw_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&asmdraw_app);
    return true;
}

static int toolbar_hit_tool(const asmdraw_state_t *s, int mx, int my) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16;
    for (int t = 0; t < NUM_TOOLS; t++) {
        int bx = wx + TOOLBAR_PAD;
        int by = wy + TOOLBAR_PAD + t * (TOOL_BTN_SZ + TOOLBAR_PAD);
        if (mx >= bx && mx < bx + TOOL_BTN_SZ && my >= by &&
            my < by + TOOL_BTN_SZ)
            return t;
    }
    return -1;
}

static int toolbar_hit_brush(const asmdraw_state_t *s, int mx, int my) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16;
    for (int b = 0; b < BRUSH_SIZES; b++) {
        int bx = wx + TOOLBAR_PAD;
        int by = wy + TOOLBAR_PAD +
                 (NUM_TOOLS + 1 + b) * (TOOL_BTN_SZ + TOOLBAR_PAD);
        if (mx >= bx && mx < bx + TOOL_BTN_SZ && my >= by &&
            my < by + TOOL_BTN_SZ)
            return b;
    }
    return -1;
}

static int palette_hit(const asmdraw_state_t *s, int mx, int my) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16;
    int ww = s->win->w - 2;
    int wh = s->win->h - 16;
    int pal_y = wy + wh - PALETTE_H;
    int cell_w = (ww - TOOLBAR_W - 2) / PALETTE_COLS;
    if (cell_w < 1)
        cell_w = 1;
    int pal_x0 = wx + TOOLBAR_W + 2;

    if (my < pal_y || my >= pal_y + PALETTE_H)
        return -1;
    int col = (mx - pal_x0) / cell_w;
    if (col < 0 || col >= PALETTE_COLS)
        return -1;
    return col;
}

static void asmdraw_init(void *state) {
    asmdraw_state_t *s = (asmdraw_state_t *)state;

    const window_spec_t spec = {
        .x = ASMDRAW_DEFAULT_X,
        .y = ASMDRAW_DEFAULT_Y,
        .w = ASMDRAW_DEFAULT_W,
        .h = ASMDRAW_DEFAULT_H,
        .min_h = ASMDRAW_DEFAULT_H,
        .min_w = ASMDRAW_DEFAULT_W / 2,
        .resizable = true,
        .title = "ASMDraw",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = LIGHT_GRAY,
        .visible = true,
        .on_close = asmdraw_close,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;

    s->win->on_draw = asmdraw_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "New", menu_new);
    menu_add_item(file_menu, "Open", menu_open);
    menu_add_item(file_menu, "Save", menu_save);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", menu_close_draw);
    menu_add_item(file_menu, "About", on_about_draw);

    s->tool = TOOL_PENCIL;
    s->brush_size = 0;
    s->fg_color = BLACK;
    s->bg_color = WHITE;
    s->fname_mode = 0;
    s->drawing = false;
    s->last_cx = -1;
    s->last_cy = -1;

    update_canvas_size(s);

    for (int y = 0; y < CANVAS_MAX_H; y++)
        for (int x = 0; x < CANVAS_MAX_W; x++)
            s->canvas[y][x] = WHITE;
}

static void asmdraw_on_frame(void *state) {
    asmdraw_state_t *s = (asmdraw_state_t *)state;
    if (!s->win || !s->win->visible)
        return;

    update_canvas_size(s);
    if (s->status_timer > 0)
        s->status_timer--;

    if (s->fname_mode != 0) {
        if (kb.key_pressed) {
            if (kb.last_scancode == ESC) {
                s->fname_mode = 0;
            } else if (kb.last_scancode == ENTER && s->fname_len > 0) {
                commit_fname(s);
            } else if (kb.last_scancode == BACKSPACE) {
                if (s->fname_len > 0)
                    s->fname_buf[--s->fname_len] = '\0';
            } else if (kb.last_char >= 32 && kb.last_char < 127 &&
                       s->fname_len < 63) {
                char c = kb.last_char;
                bool ok = (c == '/' || c == '.' || (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '_' || c == '-');
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
            int bx = wx2 + 20;
            int by = wy2 + wh2 / 2 - 20;
            bool ok_hit = (mouse.x >= bx + 4 && mouse.x < bx + 34 &&
                           mouse.y >= by + 28 && mouse.y < by + 36);
            bool can_hit = (mouse.x >= bx + 38 && mouse.x < bx + 78 &&
                            mouse.y >= by + 28 && mouse.y < by + 36);
            if (can_hit) {
                s->fname_mode = 0;
            } else if (ok_hit && s->fname_len > 0) {
                commit_fname(s);
            }
        }
        asmdraw_draw(s->win, s);
        return;
    }

    if (mouse.left_clicked) {
        int t = toolbar_hit_tool(s, mouse.x, mouse.y);
        if (t >= 0) {
            s->tool = t;
            asmdraw_draw(s->win, s);
            return;
        }

        int b = toolbar_hit_brush(s, mouse.x, mouse.y);
        if (b >= 0) {
            s->brush_size = b;
            asmdraw_draw(s->win, s);
            return;
        }

        int p = palette_hit(s, mouse.x, mouse.y);
        if (p >= 0) {
            if (kb.shift)
                s->bg_color = PALETTE_COLORS[p];
            else
                s->fg_color = PALETTE_COLORS[p];
            asmdraw_draw(s->win, s);
            return;
        }
    }
    if (mouse.right_clicked) {
        int p = palette_hit(s, mouse.x, mouse.y);
        if (p >= 0) {
            s->bg_color = PALETTE_COLORS[p];
            asmdraw_draw(s->win, s);
            return;
        }
    }

    int cx, cy;
    bool on_canvas = mouse_to_canvas(s, mouse.x, mouse.y, &cx, &cy);

    if (s->tool == TOOL_PENCIL || s->tool == TOOL_ERASER) {
        uint8_t color = (s->tool == TOOL_ERASER) ? s->bg_color : s->fg_color;
        if (mouse.left && on_canvas) {
            if (s->last_cx >= 0 && s->last_cy >= 0)
                canvas_line(s, s->last_cx, s->last_cy, cx, cy, color);
            else
                canvas_paint(s, cx, cy, color);
            s->last_cx = cx;
            s->last_cy = cy;
        } else {
            s->last_cx = -1;
            s->last_cy = -1;
        }
    } else if (s->tool == TOOL_FILL) {
        if (mouse.left_clicked && on_canvas)
            canvas_fill(s, cx, cy, s->fg_color);
    } else if (s->tool == TOOL_LINE || s->tool == TOOL_RECT) {
        if (mouse.left_clicked && on_canvas && !s->drawing) {
            s->drawing = true;
            s->start_x = cx;
            s->start_y = cy;
        } else if (!mouse.left && s->drawing) {
            if (on_canvas) {
                if (s->tool == TOOL_LINE)
                    canvas_line(s, s->start_x, s->start_y, cx, cy, s->fg_color);
                else
                    canvas_rect(s, s->start_x, s->start_y, cx, cy, s->fg_color);
            }
            s->drawing = false;
        }
    }

    asmdraw_draw(s->win, s);
}

static void asmdraw_destroy(void *state) {
    asmdraw_state_t *s = (asmdraw_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor asmdraw_app = {
    .name = "ASMDRAW",
    .state_size = sizeof(asmdraw_state_t),
    .init = asmdraw_init,
    .on_frame = asmdraw_on_frame,
    .destroy = asmdraw_destroy,
};
