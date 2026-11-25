// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "extapps/shim.h"
#include "os/prefs.h"

#define TOOL_PENCIL 0
#define TOOL_ERASER 1
#define TOOL_LINE 2
#define TOOL_RECT 3
#define TOOL_FILL 4
#define NUM_TOOLS 5
#define BRUSH_SIZES 3
#define CUSTOM APP_COLORS_CUSTOM /* custom colours, on a 256-colour screen */
#define FIRST_CUSTOM APP_COLOR_CUSTOM

static const uint8_t PALETTE_COLORS[16] = {
    BLACK,   WHITE,         DARK_GRAY, LIGHT_GRAY, RED,   LIGHT_RED,
    GREEN,   LIGHT_GREEN,   BLUE,      LIGHT_BLUE, BROWN, YELLOW,
    MAGENTA, LIGHT_MAGENTA, CYAN,      LIGHT_CYAN,
};

typedef struct {
    window *win; /* must stay first */
    control *tools[NUM_TOOLS], *brushes[BRUSH_SIZES], *canvas_ctl, *palette,
        *status;

    uint8_t *canvas; /* canvas_w * canvas_h colour bytes */
    int canvas_w, canvas_h;

    int tool, brush_size;
    uint8_t fg_color, bg_color;

    bool drawing;
    int start_x, start_y, cur_x, cur_y, last_cx, last_cy;
    char path[FS_PATH_SZ];
    char title[48];
    char dir[FS_PATH_SZ]; /* folder of the last open/save (remembered) */
    uint32_t status_until;

    /* 256 colours: the 16, then 8 custom ones (white at first) the
     * sliders change; the screen's colours shared by every window, so
     * this window's are put back when it gets the focus */
    bool many;                 /* the screen has 256 colours */
    uint8_t pal[256][3];       /* the screen's, with this window's own */
    uint8_t custom[CUSTOM][3]; /* 0..63 each */
    int edit;                  /* the custom colour the sliders change */
    control *rgb[3], *rgb_lab[3];
    bool had_focus;
} asmdraw_state_t;

/* the palette strip: 16 colours, or 24 with the custom ones */
static int swatches(const asmdraw_state_t *s) { return s->many ? 24 : 16; }
static uint8_t swatch_color(int i) {
    return i < 16 ? PALETTE_COLORS[i] : (uint8_t)(FIRST_CUSTOM + i - 16);
}

/* this window's custom colours to the screen (and to pal) */
static void apply_custom(asmdraw_state_t *s) {
    for (int k = 0; k < CUSTOM; k++) {
        for (int c = 0; c < 3; c++)
            s->pal[FIRST_CUSTOM + k][c] = s->custom[k][c];
        if (s->many)
            A->set_screen_color(FIRST_CUSTOM + k, s->custom[k][0],
                                s->custom[k][1], s->custom[k][2]);
    }
}

/* the sliders show the custom colour being changed */
static void show_rgb(asmdraw_state_t *s) {
    for (int c = 0; s->many && c < 3; c++)
        ctl_set_value(s->rgb[c], s->custom[s->edit][c]);
}

/* "NAME.PIC - ASMDraw" once a file is open or saved */
static void set_title(asmdraw_state_t *s) {
    const char *base = strrchr(s->path, '/');
    base = base ? base + 1 : s->path;
    if (!base[0])
        return;
    snprintf(s->title, sizeof(s->title), "%s - ASMDraw", base);
    s->win->title = s->title;
}

static void draw_set_status(asmdraw_state_t *s, const char *msg) {
    ctl_set_text(s->status, msg);
    s->status_until = time_millis() + 3000;
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
            s->canvas[(y)*s->canvas_w + (x)] = color;
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
    uint8_t target = s->canvas[(cy)*s->canvas_w + (cx)];
    if (target == color)
        return;

    typedef struct {
        int16_t x, y;
    } pt_t;
    int max_pts = s->canvas_w * s->canvas_h / 2;
    pt_t *stk = (pt_t *)kmalloc(max_pts * sizeof(pt_t));
    if (!stk)
        return;
    int top = 0;

    stk[top].x = (int16_t)cx;
    stk[top].y = (int16_t)cy;
    top++;

    while (top > 0) {
        top--;
        int x = stk[top].x;
        int y = stk[top].y;
        if (x < 0 || y < 0 || x >= s->canvas_w || y >= s->canvas_h)
            continue;
        if (s->canvas[(y)*s->canvas_w + (x)] != target)
            continue;
        s->canvas[(y)*s->canvas_w + (x)] = color;
        if (top + 4 <= max_pts - 1) {
            stk[top].x = (int16_t)(x + 1);
            stk[top].y = (int16_t)y;
            top++;
            stk[top].x = (int16_t)(x - 1);
            stk[top].y = (int16_t)y;
            top++;
            stk[top].x = (int16_t)x;
            stk[top].y = (int16_t)(y + 1);
            top++;
            stk[top].x = (int16_t)x;
            stk[top].y = (int16_t)(y - 1);
            top++;
        }
    }

    kfree(stk);
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
    int cw = s->canvas_ctl->aw - 4, ch = s->canvas_ctl->ah - 4;
    if (cw > g_screen_width)
        cw = g_screen_width;
    if (ch > g_screen_height)
        ch = g_screen_height;
    if (cw < 1)
        cw = 1;
    if (ch < 1)
        ch = 1;
    if (cw == s->canvas_w && ch == s->canvas_h)
        return;
    uint8_t *nb = (uint8_t *)kmalloc(cw * ch);
    if (!nb)
        return; /* keep the old size rather than corrupt */
    memset(nb, WHITE, (uint32_t)(cw * ch));
    if (s->canvas) {
        int w = cw < s->canvas_w ? cw : s->canvas_w;
        int h = ch < s->canvas_h ? ch : s->canvas_h;
        for (int y = 0; y < h; y++)
            memcpy(nb + y * cw, s->canvas + y * s->canvas_w, (uint32_t)w);
        kfree(s->canvas);
    }
    s->canvas = nb;
    s->canvas_w = cw;
    s->canvas_h = ch;
}

/* .PIC: width, height (16-bit each), then the pixels row by row. Written
 * and read in parts of up to 8 KB: the canvas may be 250 KB. Then "CUS8"
 * and the 8 custom colours (R G B, 0..63): older ASMDraws stop reading
 * before it. */
#define PART 8192
#define TRAILER "CUS8"

static bool is_bmp(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot + 1, "BMP");
}

/* uses only the 16 (a 4-bit .BMP will do) */
static bool only16(const asmdraw_state_t *s) {
    int n = s->canvas_w * s->canvas_h;
    for (int i = 0; i < n; i++)
        if (s->canvas[i] > 15)
            return false;
    return true;
}

static void asmdraw_save(asmdraw_state_t *s, const char *path) {
    update_canvas_size(s);
    if (s->canvas_w <= 0 || s->canvas_h <= 0) {
        draw_set_status(s, "Invalid canvas size.");
        return;
    }
    if (is_bmp(path)) {
        const char *e =
            bmp_save(path, s->canvas, s->canvas_w, s->canvas_h, s->canvas_w,
                     (const uint8_t (*)[3])s->pal, only16(s) ? 16 : 256);
        draw_set_status(s, e ? e : "Saved.");
        return;
    }
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(s->canvas_w & 0xFF);
    hdr[1] = (uint8_t)((s->canvas_w >> 8) & 0xFF);
    hdr[2] = (uint8_t)(s->canvas_h & 0xFF);
    hdr[3] = (uint8_t)((s->canvas_h >> 8) & 0xFF);
    bool ok = A->file_write(path, hdr, 4); /* replaces the old file */
    int total = s->canvas_w * s->canvas_h;
    for (int at = 0; ok && at < total; at += PART)
        ok = A->file_append(path, s->canvas + at,
                            total - at < PART ? total - at : PART);
    uint8_t tr[4 + CUSTOM * 3];
    memcpy(tr, TRAILER, 4);
    memcpy(tr + 4, s->custom, CUSTOM * 3);
    ok = ok && A->file_append(path, tr, sizeof(tr));
    draw_set_status(s, ok ? "Saved." : "Save failed.");
}

/* a picture's custom colours become this window's */
static void take_custom(asmdraw_state_t *s, const uint8_t (*c)[3]) {
    memcpy(s->custom, c, sizeof(s->custom));
    apply_custom(s);
    show_rgb(s);
}

/* on a 16-colour screen, colours above 15 (from a 256-colour one) become
 * the nearest of the 16 */
static void to16(asmdraw_state_t *s) {
    if (s->many)
        return;
    static uint8_t all[256][3];
    uint8_t map[256];
    A->palette256(all, 256);
    memcpy(all[FIRST_CUSTOM], s->custom, sizeof(s->custom));
    for (int i = 0; i < 256; i++) {
        int best = 0, bd = 0x7FFFFFFF;
        for (int k = 0; k < 16 && i >= 16; k++) {
            int d = 0;
            for (int c = 0; c < 3; c++) {
                int e = all[i][c] - s->pal[k][c];
                d += e * e * (c == 1 ? 4 : c == 0 ? 2 : 3);
            }
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        map[i] = (uint8_t)(i < 16 ? i : best);
    }
    int n = s->canvas_w * s->canvas_h;
    for (int i = 0; i < n; i++)
        s->canvas[i] = map[s->canvas[i]];
}

static void load_bmp(asmdraw_state_t *s, const char *path) {
    bmp_info_t *bi = (bmp_info_t *)kmalloc(sizeof(bmp_info_t));
    if (!bi) {
        draw_set_status(s, "Out of memory.");
        return;
    }
    const char *e = bmp_open(path, bi);
    if (!e) {
        /* saved by ASMOS on a 256-colour screen: its custom colours */
        bool ours = s->many && bi->bpp == 8 && bi->colors == 256;
        for (int i = 0; ours && i < FIRST_CUSTOM; i++)
            for (int c = 0; c < 3; c++)
                ours = ours && (bi->pal[i][c] >> 2) == s->pal[i][c];
        if (ours) {
            uint8_t c[CUSTOM][3];
            for (int k = 0; k < CUSTOM; k++)
                for (int j = 0; j < 3; j++)
                    c[k][j] = bi->pal[FIRST_CUSTOM + k][j] >> 2;
            take_custom(s, (const uint8_t (*)[3])c);
        }
        memset(s->canvas, WHITE, (uint32_t)(s->canvas_w * s->canvas_h));
        /* the nearest of the screen's fixed colours (not the custom ones,
         * which change), the same index where the colour is the same */
        bmp_target_t t = {(const uint8_t (*)[3])s->pal,
                          s->many ? FIRST_CUSTOM : 16, s->many ? 256 : 16,
                          false};
        e = bmp_read_all(path, bi, &t, s->canvas, s->canvas_w, s->canvas_h,
                         s->canvas_w);
    }
    static char m[64];
    if (e)
        snprintf(m, sizeof(m), "%s", e);
    else if (bi->w > s->canvas_w || bi->h > s->canvas_h)
        snprintf(m, sizeof(m), "Opened: %d x %d, cut to the window.", bi->w,
                 bi->h);
    else
        snprintf(m, sizeof(m), "Opened.");
    kfree(bi);
    draw_set_status(s, m);
}

static void asmdraw_load(asmdraw_state_t *s, const char *path) {
    if (is_bmp(path)) {
        load_bmp(s, path);
        return;
    }
    uint8_t hdr[4];
    if (A->file_read_at(path, 0, hdr, 4) < 4) {
        draw_set_status(s, "File not found.");
        return;
    }
    int fw = hdr[0] | (hdr[1] << 8);
    int fh = hdr[2] | (hdr[3] << 8);
    if (fw < 1 || fw > g_screen_width || fh < 1 || fh > g_screen_height) {
        draw_set_status(s, "Unsupported size.");
        return;
    }

    memset(s->canvas, WHITE, (uint32_t)(s->canvas_w * s->canvas_h));

    uint8_t *part = (uint8_t *)kmalloc(PART);
    if (!part) {
        draw_set_status(s, "Out of memory.");
        return;
    }
    uint32_t off = 4;
    int rows = PART / fw; /* whole rows per part */
    for (int y = 0; y < fh && y < s->canvas_h; y += rows) {
        int n = fh - y < rows ? fh - y : rows;
        int got = A->file_read_at(path, off, part, n * fw);
        if (got < fw)
            break;
        n = got / fw;
        for (int r = 0; r < n && y + r < s->canvas_h; r++)
            for (int x = 0; x < fw && x < s->canvas_w; x++)
                s->canvas[(y + r) * s->canvas_w + x] = part[r * fw + x];
        off += (uint32_t)(n * fw);
    }
    kfree(part);
    uint8_t tr[4 + CUSTOM * 3]; /* the custom colours, if it has them */
    if (A->file_read_at(path, 4 + (uint32_t)(fw * fh), tr, sizeof(tr)) ==
            (int)sizeof(tr) &&
        !memcmp(tr, TRAILER, 4))
        take_custom(s, (const uint8_t (*)[3])(tr + 4));
    to16(s);
    draw_set_status(s, "Opened.");
}

// canvas control

static void paint_canvas(control *c, int x, int y, int w, int h) {
    asmdraw_state_t *s = c->user;
    update_canvas_size(s);
    /* a row at a time: draw_dot per pixel made a 486 crawl (5 fps) */
    draw_pixels(x, y, s->canvas_w < w ? s->canvas_w : w,
                s->canvas_h < h ? s->canvas_h : h, s->canvas, s->canvas_w);
    if (s->drawing && (s->tool == TOOL_LINE || s->tool == TOOL_RECT)) {
        int x0 = x + s->start_x, y0 = y + s->start_y;
        int x1 = x + s->cur_x, y1 = y + s->cur_y;
        if (s->tool == TOOL_LINE)
            draw_line(x0, y0, x1, y1, s->fg_color);
        else
            draw_rect(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
                      (x1 > x0 ? x1 - x0 : x0 - x1) + 1,
                      (y1 > y0 ? y1 - y0 : y0 - y1) + 1, s->fg_color);
    }
}

static void on_canvas(control *c, int ev) {
    asmdraw_state_t *s = c->user;
    int cx = c->mx - 2, cy = c->my - 2; /* inside the border */
    if (cx < 0)
        cx = 0;
    if (cy < 0)
        cy = 0;
    if (cx >= s->canvas_w)
        cx = s->canvas_w - 1;
    if (cy >= s->canvas_h)
        cy = s->canvas_h - 1;
    s->cur_x = cx;
    s->cur_y = cy;
    uint8_t col = s->tool == TOOL_ERASER ? s->bg_color : s->fg_color;

    if (ev == EV_MOUSEDOWN || ev == EV_DBLCLICK) {
        switch (s->tool) {
        case TOOL_PENCIL:
        case TOOL_ERASER:
            canvas_paint(s, cx, cy, col);
            s->last_cx = cx;
            s->last_cy = cy;
            break;
        case TOOL_FILL:
            canvas_fill(s, cx, cy, s->fg_color);
            break;
        default:
            s->drawing = true;
            s->start_x = cx;
            s->start_y = cy;
        }
    } else if (ev == EV_MOUSEMOVE) {
        if (s->tool == TOOL_PENCIL || s->tool == TOOL_ERASER) {
            canvas_line(s, s->last_cx, s->last_cy, cx, cy, col);
            s->last_cx = cx;
            s->last_cy = cy;
        }
    } else if (ev == EV_MOUSEUP && s->drawing) {
        if (s->tool == TOOL_LINE)
            canvas_line(s, s->start_x, s->start_y, cx, cy, s->fg_color);
        else
            canvas_rect(s, s->start_x, s->start_y, cx, cy, s->fg_color);
        s->drawing = false;
    }
}

// palette control

static void paint_palette(control *c, int x, int y, int w, int h) {
    asmdraw_state_t *s = c->user;
    /* current colours: fg square over bg square */
    fill_rect(x + 6, y + 3, 10, h - 6, s->bg_color);
    draw_rect(x + 6, y + 3, 10, h - 6, BLACK);
    fill_rect(x + 1, y + 1, 10, h - 6, s->fg_color);
    draw_rect(x + 1, y + 1, 10, h - 6, BLACK);
    int n = swatches(s), cw = (w - 20) / n;
    for (int i = 0; i < n; i++) {
        fill_rect(x + 20 + i * cw, y + 1, cw - 1, h - 2, swatch_color(i));
        draw_rect(x + 20 + i * cw, y + 1, cw - 1, h - 2, BLACK);
    }
    if (s->many) { /* the one the sliders change: a white-black frame */
        int ex = x + 20 + (16 + s->edit) * cw;
        draw_rect(ex - 1, y, cw + 1, h, WHITE);
        draw_rect(ex, y + 1, cw - 1, h - 2, BLACK);
    }
}

static void on_palette(control *c, int ev) {
    asmdraw_state_t *s = c->user;
    if (ev != EV_MOUSEDOWN && ev != EV_DBLCLICK)
        return;
    int n = swatches(s), cw = (c->aw - 20) / n;
    int i = cw > 0 ? (c->mx - 20) / cw : -1;
    if (c->mx < 20 || i < 0 || i >= n)
        return;
    if (kb.shift)
        s->bg_color = swatch_color(i);
    else
        s->fg_color = swatch_color(i);
    if (i >= 16) { /* a custom one: the sliders change it now */
        s->edit = i - 16;
        show_rgb(s);
    }
}

static void on_rgb(control *c, int ev) {
    asmdraw_state_t *s = c->user;
    if (ev != EV_CHANGE)
        return;
    for (int k = 0; k < 3; k++)
        if (c == s->rgb[k])
            s->custom[s->edit][k] = (uint8_t)ctl_value(c);
    apply_custom(s); /* (the canvas changes with the screen's colour) */
    wm_invalidate(s->win);
}

// tool buttons

static void select_tool(asmdraw_state_t *s, int t, int b) {
    if (t >= 0)
        s->tool = t;
    if (b >= 0)
        s->brush_size = b;
    for (int i = 0; i < NUM_TOOLS; i++)
        ctl_set_value(s->tools[i], i == s->tool);
    for (int i = 0; i < BRUSH_SIZES; i++)
        ctl_set_value(s->brushes[i], i == s->brush_size);
}

static void on_tool(control *c, int ev) {
    (void)ev;
    if (c->id < 10)
        select_tool(c->user, c->id, -1);
    else
        select_tool(c->user, -1, c->id - 10);
}

// files

/* remember the folder for the next file dialog */
static void note_dir(asmdraw_state_t *s) {
    strncpy(s->dir, s->path, sizeof(s->dir) - 1);
    char *slash = strrchr(s->dir, '/');
    if (slash)
        *(slash == s->dir ? slash + 1 : slash) = 0;
}

static void saved_as(void *user, const char *path) {
    asmdraw_state_t *s = user;
    if (!path)
        return;
    strncpy(s->path, path, sizeof(s->path) - 1);
    asmdraw_save(s, s->path);
    set_title(s);
    note_dir(s);
}

static void opened(void *user, const char *path) {
    asmdraw_state_t *s = user;
    if (!path)
        return;
    strncpy(s->path, path, sizeof(s->path) - 1);
    asmdraw_load(s, s->path);
    set_title(s);
    note_dir(s);
}

static void menu_new(void) {
    asmdraw_state_t *s = ui_active_state();
    if (!s)
        return;
    memset(s->canvas, WHITE, (uint32_t)(s->canvas_w * s->canvas_h));
    s->path[0] = 0;
    draw_set_status(s, "New canvas.");
}
static void menu_open(void) {
    asmdraw_state_t *s = ui_active_state();
    if (s)
        dlg_open_file(s->win, "Open picture", s->dir, "PIC,BMP", opened, s);
}
static void menu_save_as(void) {
    asmdraw_state_t *s = ui_active_state();
    if (s)
        dlg_save_file(s->win, "Save picture", s->dir, "UNTITLED",
                      is_bmp(s->path) ? "BMP,PIC" : "PIC,BMP", saved_as, s);
}
static void menu_save(void) {
    asmdraw_state_t *s = ui_active_state();
    if (s && s->path[0])
        asmdraw_save(s, s->path);
    else
        menu_save_as();
}
static void menu_close(void) {
    asmdraw_state_t *s = ui_active_state();
    if (s)
        os_close_own_instance(s->win);
}
static void menu_print(void) {
    asmdraw_state_t *s = ui_active_state();
    if (!s || !s->canvas)
        return;
    const char *base = strrchr(s->path, '/');
    base = base ? base + 1 : (s->path[0] ? s->path : "Picture");
    print_doc_t doc = {PRINT_DOC_IMAGE, base,        NULL,        0,
                       s->canvas,       s->canvas_w, s->canvas_h, s->canvas_w};
    dlg_print(s->win, &doc);
}

static void menu_about(void) {
    dlg_message(NULL, "About ASMDraw",
                "ASMDraw v2.1\nPencil, eraser, line, rectangle, fill.\n"
                "Click a colour for the pen, Shift+click for the background. "
                "On a 256-colour screen, 8 more colours: the sliders "
                "change the one picked.\n.PIC and .BMP pictures.");
}

static void asmdraw_init(void *state) {
    asmdraw_state_t *s = state;
    /* saved settings (os/prefs.h): last size, colours, tool, folder */
    int ww = app_prefs_get_int("ASMDRAW", "w", 270);
    int wh = app_prefs_get_int("ASMDRAW", "h", 180);
    if (ww < 270 || ww > g_screen_width)
        ww = 270;
    if (wh < 180 || wh > g_screen_height - MENUBAR_H)
        wh = 180;
    s->win = app_window(s, "ASMDraw", 20, 15, ww, wh, DARK_GRAY, AW_RESIZABLE);
    if (!s->win)
        return;
    s->win->lazy_custom = true; /* the canvas only changes in handlers */
    window *w = s->win;
    static const char *TL[NUM_TOOLS] = {"P", "E", "L", "R", "F"};
    static const char *BL[BRUSH_SIZES] = {"1", "3", "5"};
    static const char *TT[NUM_TOOLS] = {"Pencil", "Eraser", "Line", "Rectangle",
                                        "Fill"};
    static const char *BT[BRUSH_SIZES] = {"Thin brush", "Medium brush",
                                          "Thick brush"};
    for (int i = 0; i < NUM_TOOLS; i++) {
        s->tools[i] = ctl_button(w, 2, 2 + i * 15, 15, 14, TL[i], i, on_tool);
        ctl_style_add(s->tools[i], CS_TOGGLE | CS_NOFOCUS);
        ctl_set_tooltip(s->tools[i], TT[i]);
    }
    for (int i = 0; i < BRUSH_SIZES; i++) {
        s->brushes[i] =
            ctl_button(w, 2, 84 + i * 15, 15, 14, BL[i], 10 + i, on_tool);
        ctl_style_add(s->brushes[i], CS_TOGGLE | CS_NOFOCUS);
        ctl_set_tooltip(s->brushes[i], BT[i]);
    }
    /* the screen's colours; with 256, a row of sliders for the custom
     * ones under the palette (the canvas 13 rows shorter) */
    s->many = A->screen_palette(s->pal, 256) >= 256;
    for (int k = 0; k < CUSTOM; k++) {
        char key[4] = {'c', (char)('0' + k), 0};
        int v = app_prefs_get_int("ASMDRAW", key, 0x3FFFF); /* white */
        s->custom[k][0] = (uint8_t)((v >> 12) & 63);
        s->custom[k][1] = (uint8_t)((v >> 6) & 63);
        s->custom[k][2] = (uint8_t)(v & 63);
    }
    apply_custom(s);
    int row = s->many ? 13 : 0;
    s->canvas_ctl =
        ctl_custom(w, 20, 2, 246, 134 - row, paint_canvas, on_canvas,
                   CS_BORDER | CS_STRETCH_W | CS_STRETCH_H);
    s->palette = ctl_custom(w, 20, 138 - row, 246, 12, paint_palette,
                            on_palette, CS_STRETCH_W | CS_ANCHOR_BOTTOM);
    ctl_set_tooltip(s->palette,
                    s->many ? "Click: pen colour, Shift+click: background. "
                              "The last 8: your own (the sliders)"
                            : "Click: pen colour, Shift+click: background");
    static const char *RGB[3] = {"R", "G", "B"};
    static const char *RGBT[3] = {"Red of the framed colour (0-63)",
                                  "Green of the framed colour (0-63)",
                                  "Blue of the framed colour (0-63)"};
    for (int c = 0; s->many && c < 3; c++) {
        s->rgb_lab[c] = ctl_label(w, 22 + c * 80, 139, 8, 11, RGB[c], 0);
        ctl_style_add(s->rgb_lab[c], CS_ANCHOR_BOTTOM);
        s->rgb[c] = ctl_slider(w, 30 + c * 80, 138, 66, 0, 63, 63, on_rgb);
        ctl_style_add(s->rgb[c], CS_ANCHOR_BOTTOM);
        ctl_set_tooltip(s->rgb[c], RGBT[c]);
    }
    show_rgb(s);
    s->status = ctl_status(w, "");

    menu *f = window_add_menu(w, "File");
    menu_add_item(f, "New", menu_new);
    menu_add_item(f, "Open...", menu_open);
    menu_add_item(f, "Save", menu_save);
    menu_add_item(f, "Save As...", menu_save_as);
    menu_add_item(f, "Print...", menu_print);
    menu_add_separator(f);
    menu_add_item(f, "Close", menu_close);
    menu_add_item(f, "About", menu_about);

    /* (a custom colour only where there are 256) */
    int pen = app_prefs_get_int("ASMDRAW", "pen", BLACK);
    int paper = app_prefs_get_int("ASMDRAW", "paper", WHITE);
    s->fg_color =
        (uint8_t)(pen >= 0 && pen < (s->many ? 256 : 16) ? pen : BLACK);
    s->bg_color =
        (uint8_t)(paper >= 0 && paper < (s->many ? 256 : 16) ? paper : WHITE);
    int tool = app_prefs_get_int("ASMDRAW", "tool", TOOL_PENCIL);
    int brush = app_prefs_get_int("ASMDRAW", "brush", 0);
    select_tool(s, tool >= 0 && tool < NUM_TOOLS ? tool : TOOL_PENCIL,
                brush >= 0 && brush < BRUSH_SIZES ? brush : 0);
    if (!app_prefs_get("ASMDRAW", "dir", s->dir, sizeof(s->dir)))
        strcpy(s->dir, "/");
    update_canvas_size(s);
}

static void opened(void *user, const char *path);

static void asmdraw_on_frame(void *state) {
    asmdraw_state_t *s = state;
    char req[FS_PATH_SZ]; /* FileF: open this picture (a new window) */
    if (s->win && app_open_request(req, sizeof(req)))
        opened(s, req);
    /* another ASMDraw may have changed the shared custom colours */
    bool focus = s->win && window_is_focused(s->win);
    if (focus && !s->had_focus && s->many)
        apply_custom(s);
    s->had_focus = focus;
    if (time_millis() > s->status_until) {
        char info[32] = "";
        control *c = s->canvas_ctl;
        if (mouse.x >= c->ax + 2 && mouse.y >= c->ay + 2 &&
            mouse.x < c->ax + 2 + s->canvas_w &&
            mouse.y < c->ay + 2 + s->canvas_h)
            snprintf(info, sizeof(info), "%d,%d", mouse.x - c->ax - 2,
                     mouse.y - c->ay - 2);
        ctl_set_text(s->status, info);
    }
}

static void asmdraw_destroy(void *state) {
    asmdraw_state_t *s = state;
    if (s->win) {
        app_prefs_begin("ASMDRAW");
        app_prefs_set_int("ASMDRAW", "pen", s->fg_color);
        app_prefs_set_int("ASMDRAW", "paper", s->bg_color);
        app_prefs_set_int("ASMDRAW", "tool", s->tool);
        app_prefs_set_int("ASMDRAW", "brush", s->brush_size);
        app_prefs_set_int("ASMDRAW", "w", s->win->w);
        app_prefs_set_int("ASMDRAW", "h", s->win->h);
        app_prefs_set("ASMDRAW", "dir", s->dir);
        for (int k = 0; k < CUSTOM; k++) {
            char key[4] = {'c', (char)('0' + k), 0};
            app_prefs_set_int("ASMDRAW", key,
                              s->custom[k][0] << 12 | s->custom[k][1] << 6 |
                                  s->custom[k][2]);
        }
        app_prefs_end();
    }
    if (s->canvas)
        kfree(s->canvas);
    s->canvas = NULL;
}

/* the canvas (up to the screen, 250 KB at 640x400) lives in the heap, and
 * a .BMP being read (40 KB more) */
APP_META("ASMDraw", asmdraw_state_t, false, 320, asmdraw_init, asmdraw_on_frame,
         asmdraw_destroy);
