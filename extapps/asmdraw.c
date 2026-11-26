// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* ASMDraw: a paint program.
 *
 *   tools (2 columns)   | the canvas
 *   brush sizes          |
 *   pen/paper | colours (16; 24 with a 256-colour screen) [Edit...]
 *
 * Shapes are drawn with the brush: lines, rectangles and ellipses, empty
 * or filled (Shift: straight lines at 45 degrees, squares, circles). The
 * 8 custom colours are changed with the colour picker (dlg_color). */

#include "extapps/shim.h"
#include "os/prefs.h"

/* tools: the numbers are kept in the settings (0..4 as before) */
#define TOOL_PENCIL 0
#define TOOL_ERASER 1
#define TOOL_LINE 2
#define TOOL_RECT 3
#define TOOL_FILL 4
#define TOOL_FRECT 5
#define TOOL_ELLIPSE 6
#define TOOL_FELLIPSE 7
#define TOOL_SPRAY 8
#define TOOL_PICK 9
#define TOOL_SELECT 10
#define NUM_TOOLS 11
#define NUM_SIZES 6
#define CUSTOM APP_COLORS_CUSTOM /* custom colours, on a 256-colour screen */
#define FIRST_CUSTOM APP_COLOR_CUSTOM
#define BTN 15            /* a tool button */
#define SBTN 13           /* a brush size button (a row) */
#define BAR (2 * BTN + 4) /* the tool bar's width */
#define STRIP 12          /* the colour strip's height */
#define FILL_STACK 16384  /* the fill's pending runs */

static const uint8_t PALETTE_COLORS[16] = {
    BLACK,   WHITE,         DARK_GRAY, LIGHT_GRAY, RED,   LIGHT_RED,
    GREEN,   LIGHT_GREEN,   BLUE,      LIGHT_BLUE, BROWN, YELLOW,
    MAGENTA, LIGHT_MAGENTA, CYAN,      LIGHT_CYAN,
};
static const uint8_t SIZES[NUM_SIZES] = {1, 2, 3, 5, 7, 9}; /* pixels */
/* the buttons, in the bar's order (two a row) */
static const uint8_t BAR_TOOLS[NUM_TOOLS] = {
    TOOL_SELECT, TOOL_PICK, TOOL_PENCIL, TOOL_ERASER,  TOOL_FILL,    TOOL_SPRAY,
    TOOL_LINE,   TOOL_RECT, TOOL_FRECT,  TOOL_ELLIPSE, TOOL_FELLIPSE};
static const char *const TOOL_NAMES[NUM_TOOLS] = {
    "Pencil",
    "Eraser (the paper colour)",
    "Line (Shift: straight or 45 degrees)",
    "Rectangle (Shift: a square)",
    "Fill an area",
    "Filled rectangle (Shift: a square)",
    "Ellipse (Shift: a circle)",
    "Filled ellipse (Shift: a circle)",
    "Spray",
    "Pick a colour from the picture",
    "Select: drag a box, then drag it to move it (Ctrl: a copy); "
    "right-click: cut, copy, paste"};

/* 11x11 pictures: '#' black, 'o' the pen's colour, '.' nothing */
static const char *const ICONS[NUM_TOOLS][11] = {
    {"........##.", ".......#..#", "......#..#.", ".....#..#..", "....#..#...",
     "...#..#....", "..#..#.....", ".##.#......", ".#.#.......", "###........",
     "##........."}, /* pencil */
    {"...........", "......####.", ".....#..##.", "....#..#.#.", "...#..#..#.",
     "..####...#.", "..#..#..#..", "..#..#.#...", "..####.....", "...........",
     "..........."}, /* eraser */
    {"...........", ".........#.", "........#..", ".......#...", "......#....",
     ".....#.....", "....#......", "...#.......", "..#........", ".#.........",
     "..........."}, /* line */
    {"...........", "...........", ".#########.", ".#.......#.", ".#.......#.",
     ".#.......#.", ".#.......#.", ".#.......#.", ".#########.", "...........",
     "..........."}, /* rectangle */
    {"....#......", "...#.#.....", "..#...#....", ".#.....#...", "#.......#..",
     ".#.....#o..", "..#...#.o..", "...#.#..oo.", "....#...oo.", "...........",
     "..........."}, /* fill */
    {"...........", "...........", ".#########.", ".#ooooooo#.", ".#ooooooo#.",
     ".#ooooooo#.", ".#ooooooo#.", ".#ooooooo#.", ".#########.", "...........",
     "..........."}, /* filled rectangle */
    {"...........", "...#####...", "..#.....#..", ".#.......#.", ".#.......#.",
     ".#.......#.", ".#.......#.", ".#.......#.", "..#.....#..", "...#####...",
     "..........."}, /* ellipse */
    {"...........", "...#####...", "..#ooooo#..", ".#ooooooo#.", ".#ooooooo#.",
     ".#ooooooo#.", ".#ooooooo#.", ".#ooooooo#.", "..#ooooo#..", "...#####...",
     "..........."}, /* filled ellipse */
    {"......o.o.o", ".......o.o.", ".###..o.o.o", ".#.#.......", "#####......",
     "#...#......", "#...#......", "#...#......", "#...#......", "#####......",
     "..........."}, /* spray */
    {".........##", "........###", ".......###.", "......##...", ".....#.#...",
     "....#.#....", "...#.#.....", "..#.#......", ".#.#.......", ".##........",
     "#.........."}, /* pick */
    {"...........", ".#.#.#.#.#.", "...........", ".#.......#.", "...........",
     ".#.......#.", "...........", ".#.......#.", "...........", ".#.#.#.#.#.",
     "..........."}, /* select */
};

typedef struct {
    window *win; /* must stay first */
    control *tools[NUM_TOOLS], *sizes[NUM_SIZES], *canvas_ctl, *palette, *pens,
        *edit_btn, *status;

    uint8_t *canvas; /* canvas_w * canvas_h colour bytes */
    int canvas_w, canvas_h;

    int tool, size, before_pick;       /* (the tool to go back to after Pick) */
    uint8_t fg_color, bg_color, color; /* color: the stroke's */

    bool drawing, shift;
    int start_x, start_y, cur_x, cur_y, last_cx, last_cy;
    uint32_t seed; /* the spray's dots */
    char path[FS_PATH_SZ];
    char title[48];
    char dir[FS_PATH_SZ]; /* folder of the last open/save (remembered) */
    uint32_t status_until;

    /* 256 colours: the 16, then 8 custom ones (white at first) changed
     * with the colour picker; the screen's colours are shared by every
     * window, so this window's are put back when it gets the focus */
    bool many;                 /* the screen has 256 colours */
    uint8_t pal[256][3];       /* the screen's, with this window's own */
    uint8_t custom[CUSTOM][3]; /* 0..63 each */
    int edit;                  /* the custom colour Edit... changes */
    bool had_focus;

    /* the selection (TOOL_SELECT): a box on the canvas. Once moved, or
     * pasted, its pixels float over the canvas until put down. */
    bool sel;               /* there is one */
    int sx, sy, sw, sh;     /* where, in canvas pixels (may stick out) */
    uint8_t *floating;      /* its pixels while lifted (sw x sh), or NULL */
    bool selecting, moving; /* dragging a box out; dragging it */
    int grab_x, grab_y;     /* the pointer within it, while moving */
    int menu_x, menu_y;     /* where the right-click was (paste there) */
    bool menu_at;
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

// drawing: everything is runs of one row ("spans"), into the canvas or,
// while a shape is being dragged, onto the screen over it

typedef struct {
    asmdraw_state_t *s;
    bool screen;        /* onto the screen (a preview) */
    int ox, oy, vw, vh; /* the screen: where the canvas is, what shows */
    uint8_t color;
} pen_t;

static void span(const pen_t *p, int x0, int x1, int y) {
    asmdraw_state_t *s = p->s;
    int w = p->screen ? p->vw : s->canvas_w;
    int h = p->screen ? p->vh : s->canvas_h;
    if (y < 0 || y >= h)
        return;
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 >= w ? w - 1 : x1;
    if (x0 > x1)
        return;
    if (p->screen)
        fill_rect(p->ox + x0, p->oy + y, x1 - x0 + 1, 1, p->color);
    else
        memset(s->canvas + y * s->canvas_w + x0, p->color,
               (uint32_t)(x1 - x0 + 1));
}

/* the brush (round, SIZES[size] across) with its middle at (x, y): 1 and
 * 2 are squares, the rest odd and round */
static void stamp(const pen_t *p, int x, int y) {
    int d = SIZES[p->s->size];
    if (d <= 2) {
        for (int j = 0; j < d; j++)
            span(p, x, x + d - 1, y + j);
        return;
    }
    int r = (d - 1) / 2;
    for (int dy = -r; dy <= r; dy++) {
        int hw = 0;
        while ((hw + 1) * (hw + 1) + dy * dy <= r * r + r)
            hw++;
        span(p, x - hw, x + hw, y + dy);
    }
}

static void line(const pen_t *p, int x0, int y0, int x1, int y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        stamp(p, x0, y0);
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

static void rect(const pen_t *p, int x0, int y0, int x1, int y1, bool fill) {
    if (fill) {
        for (int y = y0 < y1 ? y0 : y1; y <= (y0 < y1 ? y1 : y0); y++)
            span(p, x0, x1, y);
        return;
    }
    line(p, x0, y0, x1, y0);
    line(p, x1, y0, x1, y1);
    line(p, x1, y1, x0, y1);
    line(p, x0, y1, x0, y0);
}

/* the ellipse in the box (x0, y0)-(x1, y1), A. Zingl's way: even sizes
 * too; filled, a run between each pair of points */
static void ellipse(const pen_t *p, int x0, int y0, int x1, int y1, bool fill) {
    long long a = x1 > x0 ? x1 - x0 : x0 - x1, b = y1 > y0 ? y1 - y0 : y0 - y1;
    long long b1 = b & 1;
    long long dx = 4 * (1 - a) * b * b, dy = 4 * (b1 + 1) * a * a;
    long long err = dx + dy + b1 * a * a, e2;
    if (x0 > x1) {
        x0 = x1;
        x1 += (int)a;
    }
    if (y0 > y1)
        y0 = y1;
    y0 += (int)((b + 1) / 2);
    y1 = y0 - (int)b1;
    a *= 8 * a;
    b1 = 8 * b * b;
    do {
        if (fill) {
            span(p, x0, x1, y0);
            span(p, x0, x1, y1);
        } else {
            stamp(p, x1, y0);
            stamp(p, x0, y0);
            stamp(p, x0, y1);
            stamp(p, x1, y1);
        }
        e2 = 2 * err;
        if (e2 <= dy) {
            y0++;
            y1--;
            err += dy += a;
        }
        if (e2 >= dx || 2 * err > dy) {
            x0++;
            x1--;
            err += dx += b1;
        }
    } while (x0 <= x1);
    while (y0 - y1 <= b) { /* a flat one: the ends */
        if (fill) {
            span(p, x0 - 1, x1 + 1, y0++);
            span(p, x0 - 1, x1 + 1, y1--);
        } else {
            stamp(p, x0 - 1, y0);
            stamp(p, x1 + 1, y0++);
            stamp(p, x0 - 1, y1);
            stamp(p, x1 + 1, y1--);
        }
    }
}

/* Shift: lines at 0/45/90 degrees, squares and circles */
static void constrain(const asmdraw_state_t *s, int *x1, int *y1) {
    int dx = *x1 - s->start_x, dy = *y1 - s->start_y;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (!s->shift)
        return;
    if (s->tool == TOOL_LINE) {
        if (ax > 2 * ay)
            *y1 = s->start_y;
        else if (ay > 2 * ax)
            *x1 = s->start_x;
        else {
            int m = ax > ay ? ax : ay;
            *x1 = s->start_x + (dx < 0 ? -m : m);
            *y1 = s->start_y + (dy < 0 ? -m : m);
        }
        return;
    }
    int m = ax > ay ? ax : ay;
    *x1 = s->start_x + (dx < 0 ? -m : m);
    *y1 = s->start_y + (dy < 0 ? -m : m);
}

/* the shape being dragged, from start to (x1, y1) */
static void shape(const pen_t *p, int x1, int y1) {
    const asmdraw_state_t *s = p->s;
    int x0 = s->start_x, y0 = s->start_y;
    switch (s->tool) {
    case TOOL_LINE:
        line(p, x0, y0, x1, y1);
        break;
    case TOOL_RECT:
    case TOOL_FRECT:
        rect(p, x0, y0, x1, y1, s->tool == TOOL_FRECT);
        break;
    default:
        ellipse(p, x0, y0, x1, y1, s->tool == TOOL_FELLIPSE);
    }
}

static bool is_shape(int t) {
    return t == TOOL_LINE || t == TOOL_RECT || t == TOOL_FRECT ||
           t == TOOL_ELLIPSE || t == TOOL_FELLIPSE;
}

/* the area of one colour around (cx, cy), row by row: runs to the left
 * and right, then the rows above and below from a stack of seeds */
static bool canvas_fill(asmdraw_state_t *s, int cx, int cy, uint8_t color) {
    int w = s->canvas_w, h = s->canvas_h;
    uint8_t *c = s->canvas, target = c[cy * w + cx];
    if (target == color)
        return true;
    typedef struct {
        int16_t x, y;
    } pt_t;
    pt_t *stk = (pt_t *)kmalloc(FILL_STACK * sizeof(pt_t));
    if (!stk)
        return false;
    int top = 0;
    bool whole = true;
    stk[top].x = (int16_t)cx;
    stk[top++].y = (int16_t)cy;
    while (top > 0) {
        top--;
        int x = stk[top].x, y = stk[top].y;
        if (c[y * w + x] != target)
            continue;
        int l = x, r = x;
        while (l > 0 && c[y * w + l - 1] == target)
            l--;
        while (r < w - 1 && c[y * w + r + 1] == target)
            r++;
        memset(c + y * w + l, color, (uint32_t)(r - l + 1));
        for (int ny = y - 1; ny <= y + 1; ny += 2) { /* a seed per run */
            if (ny < 0 || ny >= h)
                continue;
            bool in = false;
            for (int i = l; i <= r; i++) {
                bool t = c[ny * w + i] == target;
                if (t && !in) {
                    if (top < FILL_STACK) {
                        stk[top].x = (int16_t)i;
                        stk[top++].y = (int16_t)ny;
                    } else {
                        whole = false;
                    }
                }
                in = t;
            }
        }
    }
    kfree(stk);
    return whole;
}

/* a few dots in a circle around (x, y) */
static void spray(asmdraw_state_t *s, int x, int y) {
    pen_t p = {s, false, 0, 0, 0, 0, s->color};
    int r = SIZES[s->size] * 2 + 3, n = SIZES[s->size] + 4;
    for (int i = 0; i < n; i++) {
        int dx, dy;
        do {
            s->seed = s->seed * 1103515245u + 12345u;
            dx = (int)((s->seed >> 8) % (uint32_t)(2 * r + 1)) - r;
            s->seed = s->seed * 1103515245u + 12345u;
            dy = (int)((s->seed >> 8) % (uint32_t)(2 * r + 1)) - r;
        } while (dx * dx + dy * dy > r * r);
        span(&p, x + dx, x + dx, y + dy);
    }
}

// the selection: a box; dragged, its pixels are lifted off the canvas
// (the paper colour left behind) and float until they are put down

static void select_tool(asmdraw_state_t *s, int t, int size);

static bool in_sel(const asmdraw_state_t *s, int x, int y) {
    return s->sel && x >= s->sx && x < s->sx + s->sw && y >= s->sy &&
           y < s->sy + s->sh;
}

/* the canvas's pixel, or the paper colour off it */
static uint8_t at(const asmdraw_state_t *s, int x, int y) {
    if (x < 0 || y < 0 || x >= s->canvas_w || y >= s->canvas_h)
        return s->bg_color;
    return s->canvas[y * s->canvas_w + x];
}

/* the floating pixels into the canvas where they are (what sticks out
 * is lost) */
static void stamp_floating(asmdraw_state_t *s) {
    for (int y = 0; s->floating && y < s->sh; y++) {
        int cy = s->sy + y;
        if (cy < 0 || cy >= s->canvas_h)
            continue;
        for (int x = 0; x < s->sw; x++) {
            int cx = s->sx + x;
            if (cx >= 0 && cx < s->canvas_w)
                s->canvas[cy * s->canvas_w + cx] = s->floating[y * s->sw + x];
        }
    }
}

/* put it down: no selection any more */
static void put_down(asmdraw_state_t *s) {
    stamp_floating(s);
    kfree(s->floating);
    s->floating = NULL;
    s->sel = s->selecting = s->moving = false;
    wm_invalidate(s->win);
}

/* lift the pixels under the box; the paper colour is left, unless it is
 * a copy (Ctrl). Already floating, a copy leaves one behind. */
static bool lift(asmdraw_state_t *s, bool copy) {
    if (s->floating) {
        if (copy)
            stamp_floating(s);
        return true;
    }
    uint8_t *f = (uint8_t *)kmalloc((uint32_t)(s->sw * s->sh));
    if (!f) {
        draw_set_status(s, "Not enough memory to move it.");
        return false;
    }
    for (int y = 0; y < s->sh; y++)
        for (int x = 0; x < s->sw; x++) {
            f[y * s->sw + x] = at(s, s->sx + x, s->sy + y);
            int cx = s->sx + x, cy = s->sy + y;
            if (!copy && cx >= 0 && cy >= 0 && cx < s->canvas_w &&
                cy < s->canvas_h)
                s->canvas[cy * s->canvas_w + cx] = s->bg_color;
        }
    s->floating = f;
    return true;
}

/* the selected pixels to the clipboard (with this window's colours) */
static bool sel_copy(asmdraw_state_t *s) {
    if (!s->sel) {
        draw_set_status(s, "Nothing selected: drag a box with the Select "
                           "tool.");
        return false;
    }
    uint8_t *tmp = NULL;
    const uint8_t *pix = s->floating;
    if (!pix) { /* (the canvas under the box) */
        tmp = (uint8_t *)kmalloc((uint32_t)(s->sw * s->sh));
        for (int y = 0; tmp && y < s->sh; y++)
            for (int x = 0; x < s->sw; x++)
                tmp[y * s->sw + x] = at(s, s->sx + x, s->sy + y);
        pix = tmp;
    }
    bool ok = pix && clipboard_set_image(s->sw, s->sh, pix, s->sw,
                                         (const uint8_t (*)[3])s->pal);
    kfree(tmp);
    draw_set_status(s, ok ? "Copied." : "Too big to copy, or no memory.");
    return ok;
}

/* the selection goes: the paper colour where it was */
static void sel_delete(asmdraw_state_t *s) {
    if (!s->sel)
        return;
    if (!s->floating) /* (lifted, it is already gone from there) */
        for (int y = 0; y < s->sh; y++)
            for (int x = 0; x < s->sw; x++) {
                int cx = s->sx + x, cy = s->sy + y;
                if (cx >= 0 && cy >= 0 && cx < s->canvas_w && cy < s->canvas_h)
                    s->canvas[cy * s->canvas_w + cx] = s->bg_color;
            }
    kfree(s->floating);
    s->floating = NULL;
    s->sel = s->moving = s->selecting = false;
    wm_invalidate(s->win);
}

static void sel_cut(asmdraw_state_t *s) {
    if (sel_copy(s))
        sel_delete(s);
}

static void sel_all(asmdraw_state_t *s) {
    select_tool(s, TOOL_SELECT, -1);
    put_down(s);
    s->sel = true;
    s->sx = s->sy = 0;
    s->sw = s->canvas_w;
    s->sh = s->canvas_h;
}

/* the clipboard's picture, floating: at the right-click, or the top left.
 * Its colours: the same index where this window's colour is the same,
 * else the nearest of the screen's (not the custom ones, which change) */
static void sel_paste(asmdraw_state_t *s) {
    const clip_image_t *im = clipboard_image();
    if (!im) {
        draw_set_status(s, clipboard_kind() == CLIP_TEXT
                               ? "The clipboard holds text, not a picture."
                               : "Nothing to paste: copy a picture first.");
        return;
    }
    uint8_t *f = (uint8_t *)kmalloc((uint32_t)(im->w * im->h));
    if (!f) {
        draw_set_status(s, "Not enough memory to paste it.");
        return;
    }
    static bool used[256];
    uint8_t lut[256];
    memset(used, 0, sizeof(used));
    for (int i = 0; i < im->w * im->h; i++)
        used[im->pix[i]] = true;
    int keep = s->many ? 256 : 16, n = s->many ? FIRST_CUSTOM : 16;
    for (int i = 0; i < 256; i++) {
        lut[i] = (uint8_t)i;
        if (!used[i] || (i < keep && !memcmp(im->pal[i], s->pal[i], 3)))
            continue;
        int best = 0, bd = 0x7FFFFFFF;
        for (int k = 0; k < n; k++) {
            int d = 0;
            for (int c = 0; c < 3; c++) {
                int e = im->pal[i][c] - s->pal[k][c];
                d += e * e * (c == 1 ? 4 : c == 0 ? 2 : 3);
            }
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        lut[i] = (uint8_t)best;
    }
    for (int i = 0; i < im->w * im->h; i++)
        f[i] = lut[im->pix[i]];
    select_tool(s, TOOL_SELECT, -1);
    put_down(s);
    s->floating = f;
    s->sel = true;
    s->sw = im->w;
    s->sh = im->h;
    s->sx = s->menu_at ? s->menu_x : 0;
    s->sy = s->menu_at ? s->menu_y : 0;
    s->menu_at = false;
    draw_set_status(s, "Pasted: drag it into place, click outside to put it "
                       "down.");
}

/* Edit menu, the right-click menu (they act on the focused ASMDraw) */
#define SEL_ACTION(name, body)                                                 \
    static void name(void) {                                                   \
        asmdraw_state_t *s = ui_active_state();                                \
        if (s)                                                                 \
            body;                                                              \
    }
SEL_ACTION(menu_cut, sel_cut(s))
SEL_ACTION(menu_copy, sel_copy(s))
SEL_ACTION(menu_paste, sel_paste(s))
SEL_ACTION(menu_delete, sel_delete(s))
SEL_ACTION(menu_select_all, sel_all(s))

/* on the screen at (x, y), vw x vh of the canvas shown: the floating
 * pixels, then a dotted frame on the box's edge */
static void draw_selection(asmdraw_state_t *s, int x, int y, int vw, int vh) {
    if (!s->sel)
        return;
    if (s->floating) {
        int x0 = s->sx < 0 ? -s->sx : 0, y0 = s->sy < 0 ? -s->sy : 0;
        int x1 = vw - s->sx < s->sw ? vw - s->sx : s->sw;
        int y1 = vh - s->sy < s->sh ? vh - s->sy : s->sh;
        if (x1 > x0 && y1 > y0)
            draw_pixels(x + s->sx + x0, y + s->sy + y0, x1 - x0, y1 - y0,
                        s->floating + y0 * s->sw + x0, s->sw);
    }
    int bx = s->sx, by = s->sy, r = s->sx + s->sw - 1, b = s->sy + s->sh - 1;
    for (int i = bx; i <= r; i++)
        for (int k = 0; k < 2; k++) {
            int py = k ? b : by;
            if (i >= 0 && i < vw && py >= 0 && py < vh)
                draw_dot(x + i, y + py, ((i + py) >> 1) & 1 ? BLACK : WHITE);
        }
    for (int j = by; j <= b; j++)
        for (int k = 0; k < 2; k++) {
            int px = k ? r : bx;
            if (j >= 0 && j < vh && px >= 0 && px < vw)
                draw_dot(x + px, y + j, ((px + j) >> 1) & 1 ? BLACK : WHITE);
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
    if (s->sel)
        put_down(s); /* (a floating selection is part of the picture) */
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
    wm_invalidate(s->win);
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

// the canvas

static void paint_canvas(control *c, int x, int y, int w, int h) {
    asmdraw_state_t *s = c->user;
    update_canvas_size(s);
    int vw = s->canvas_w < w ? s->canvas_w : w;
    int vh = s->canvas_h < h ? s->canvas_h : h;
    /* a row at a time: draw_dot per pixel made a 486 crawl (5 fps) */
    draw_pixels(x, y, vw, vh, s->canvas, s->canvas_w);
    draw_selection(s, x, y, vw, vh);
    if (s->drawing && is_shape(s->tool)) { /* the shape so far */
        pen_t p = {s, true, x, y, vw, vh, s->color};
        int x1 = s->cur_x, y1 = s->cur_y;
        constrain(s, &x1, &y1);
        shape(&p, x1, y1);
    }
}

static void select_tool(asmdraw_state_t *s, int t, int size);

/* right-click: Cut, Copy, Paste (there), Delete, Select all */
static void canvas_menu(asmdraw_state_t *s, control *c, int cx, int cy) {
    s->menu_x = cx;
    s->menu_y = cy;
    s->menu_at = !ctl_menu_from_key();
    menu *m = popup_menu();
    menu_add_item(m, "Cut", menu_cut);
    menu_enable_last(m, s->sel);
    menu_add_item(m, "Copy", menu_copy);
    menu_enable_last(m, s->sel);
    menu_add_item(m, "Paste", menu_paste);
    menu_enable_last(m, clipboard_kind() == CLIP_IMAGE);
    menu_add_item(m, "Delete", menu_delete);
    menu_enable_last(m, s->sel);
    menu_add_separator(m);
    menu_add_item(m, "Select all", menu_select_all);
    popup_show_for(c);
}

/* Ctrl+X, C, V, A; Del; Esc puts the selection down */
static void canvas_key(asmdraw_state_t *s) {
    uint8_t k = kb.last_scancode;
    if (kb.ctrl && k == 0x2D)
        sel_cut(s);
    else if (kb.ctrl && k == 0x2E)
        sel_copy(s);
    else if (kb.ctrl && k == 0x2F) {
        s->menu_at = false;
        sel_paste(s);
    } else if (kb.ctrl && k == 0x1E)
        sel_all(s);
    else if (k == DELETE)
        sel_delete(s);
    else if (k == ESC && s->sel)
        put_down(s);
    else
        return;
    wm_invalidate(s->win);
}

/* the Select tool: a box dragged out, or (inside it) the box moved */
static void select_mouse(asmdraw_state_t *s, int ev, int rx, int ry, int cx,
                         int cy) {
    if (ev == EV_MOUSEDOWN || ev == EV_DBLCLICK) {
        if (in_sel(s, rx, ry)) {
            if (lift(s, kb.ctrl)) {
                s->moving = true;
                s->grab_x = rx - s->sx;
                s->grab_y = ry - s->sy;
            }
            return;
        }
        put_down(s);
        s->selecting = s->sel = true;
        s->start_x = s->sx = cx;
        s->start_y = s->sy = cy;
        s->sw = s->sh = 1;
    } else if (ev == EV_MOUSEMOVE && s->moving) {
        s->sx = rx - s->grab_x;
        s->sy = ry - s->grab_y;
    } else if (ev == EV_MOUSEMOVE && s->selecting) {
        s->sx = cx < s->start_x ? cx : s->start_x;
        s->sy = cy < s->start_y ? cy : s->start_y;
        s->sw = (cx > s->start_x ? cx - s->start_x : s->start_x - cx) + 1;
        s->sh = (cy > s->start_y ? cy - s->start_y : s->start_y - cy) + 1;
    } else if (ev == EV_MOUSEUP) {
        if (s->selecting && s->sw == 1 && s->sh == 1)
            s->sel = false; /* a click, not a box: nothing selected */
        s->selecting = s->moving = false;
    }
}

static void on_canvas(control *c, int ev) {
    asmdraw_state_t *s = c->user;
    int rx = c->mx - 2, ry = c->my - 2; /* inside the border */
    int cx = rx < 0 ? 0 : rx >= s->canvas_w ? s->canvas_w - 1 : rx;
    int cy = ry < 0 ? 0 : ry >= s->canvas_h ? s->canvas_h - 1 : ry;
    if (ev == EV_MENU) {
        canvas_menu(s, c, cx, cy);
        return;
    }
    if (ev == EV_KEY) {
        canvas_key(s);
        return;
    }
    if (s->tool == TOOL_SELECT) {
        select_mouse(s, ev, rx, ry, cx, cy);
        return;
    }
    s->cur_x = cx;
    s->cur_y = cy;
    s->shift = kb.shift;
    pen_t p = {s, false, 0, 0, 0, 0, s->color};

    if (ev == EV_MOUSEDOWN || ev == EV_DBLCLICK) {
        s->color = s->tool == TOOL_ERASER ? s->bg_color : s->fg_color;
        p.color = s->color;
        s->start_x = s->last_cx = cx;
        s->start_y = s->last_cy = cy;
        switch (s->tool) {
        case TOOL_PENCIL:
        case TOOL_ERASER:
            stamp(&p, cx, cy);
            s->drawing = true;
            break;
        case TOOL_FILL:
            if (!canvas_fill(s, cx, cy, s->fg_color))
                draw_set_status(s, "Filled what it could: the area has too "
                                   "many edges.");
            break;
        case TOOL_PICK: {
            uint8_t v = s->canvas[cy * s->canvas_w + cx];
            if (kb.shift)
                s->bg_color = v;
            else
                s->fg_color = v;
            select_tool(s, s->before_pick, -1); /* back to drawing */
            wm_invalidate(s->win);
            break;
        }
        case TOOL_SPRAY:
            spray(s, cx, cy);
            s->drawing = true;
            break;
        default: /* a shape: from here */
            s->drawing = true;
        }
    } else if (ev == EV_MOUSEMOVE && s->drawing) {
        if (s->tool == TOOL_PENCIL || s->tool == TOOL_ERASER) {
            line(&p, s->last_cx, s->last_cy, cx, cy);
            s->last_cx = cx;
            s->last_cy = cy;
        } else if (s->tool == TOOL_SPRAY) {
            spray(s, cx, cy);
        }
    } else if (ev == EV_MOUSEUP && s->drawing) {
        if (is_shape(s->tool)) {
            int x1 = cx, y1 = cy;
            constrain(s, &x1, &y1);
            shape(&p, x1, y1);
        }
        s->drawing = false;
    }
}

// the tool bar: a button each (drawn here), tools then brush sizes

#define SIZE_ID 100 /* a size button's id: SIZE_ID + its number */

static void bevel(int x, int y, int w, int h, bool down) {
    fill_rect(x, y, w, h, down ? WHITE : LIGHT_GRAY);
    draw_rect(x, y, w, h, BLACK);
    uint8_t hi = down ? DARK_GRAY : WHITE, lo = down ? LIGHT_GRAY : DARK_GRAY;
    draw_line(x + 1, y + 1, x + w - 2, y + 1, hi);
    draw_line(x + 1, y + 1, x + 1, y + h - 2, hi);
    draw_line(x + 1, y + h - 2, x + w - 2, y + h - 2, lo);
    draw_line(x + w - 2, y + 1, x + w - 2, y + h - 2, lo);
}

static void paint_button(control *c, int x, int y, int w, int h) {
    asmdraw_state_t *s = c->user;
    bool size = c->id >= SIZE_ID;
    int k = size ? c->id - SIZE_ID : c->id;
    bevel(x, y, w, h, size ? s->size == k : s->tool == k);
    if (size) { /* a dot as big as the brush */
        int d = SIZES[k], ox = x + (w - d) / 2, oy = y + (h - d) / 2;
        int r = (d - 1) / 2;
        for (int j = 0; j < d; j++) {
            int hw = r, dy = j - r;
            if (d > 2)
                for (hw = 0; (hw + 1) * (hw + 1) + dy * dy <= r * r + r;)
                    hw++;
            fill_rect(d > 2 ? ox + r - hw : ox, oy + j, d > 2 ? 2 * hw + 1 : d,
                      1, BLACK);
        }
        return;
    }
    int ix = x + (w - 11) / 2, iy = y + (h - 11) / 2;
    for (int j = 0; j < 11; j++)
        for (int i = 0; i < 11; i++) {
            char ch = ICONS[k][j][i];
            if (ch != '.')
                draw_dot(ix + i, iy + j, ch == '#' ? BLACK : s->fg_color);
        }
}

static void select_tool(asmdraw_state_t *s, int t, int size) {
    if (t >= 0 && t < NUM_TOOLS && t != TOOL_SELECT && s->sel)
        put_down(s);
    if (t >= 0 && t < NUM_TOOLS) {
        if (t == TOOL_PICK && s->tool != TOOL_PICK)
            s->before_pick = s->tool;
        s->tool = t;
    }
    if (size >= 0 && size < NUM_SIZES)
        s->size = size;
    s->drawing = false;
    wm_invalidate(s->win);
}

static void on_button(control *c, int ev) {
    asmdraw_state_t *s = c->user;
    if (ev != EV_MOUSEDOWN && ev != EV_DBLCLICK)
        return;
    if (c->id >= SIZE_ID)
        select_tool(s, -1, c->id - SIZE_ID);
    else
        select_tool(s, c->id, -1);
}

// colours

/* the pen over the paper; a click swaps them */
static void paint_pens(control *c, int x, int y, int w, int h) {
    asmdraw_state_t *s = c->user;
    int bw = w * 2 / 3, bh = h - 3;
    fill_rect(x + w - bw, y + 3, bw, bh, s->bg_color);
    draw_rect(x + w - bw, y + 3, bw, bh, BLACK);
    fill_rect(x, y, bw, bh, s->fg_color);
    draw_rect(x, y, bw, bh, BLACK);
}

static void on_pens(control *c, int ev) {
    asmdraw_state_t *s = c->user;
    if (ev != EV_MOUSEDOWN && ev != EV_DBLCLICK)
        return;
    uint8_t t = s->fg_color;
    s->fg_color = s->bg_color;
    s->bg_color = t;
    wm_invalidate(s->win);
}

static void paint_palette(control *c, int x, int y, int w, int h) {
    asmdraw_state_t *s = c->user;
    int n = swatches(s), cw = w / n;
    for (int i = 0; i < n; i++) {
        fill_rect(x + i * cw, y, cw - 1, h, swatch_color(i));
        draw_rect(x + i * cw, y, cw - 1, h, BLACK);
    }
    if (s->many) { /* the one Edit... changes: a white-black frame */
        int ex = x + (16 + s->edit) * cw;
        draw_rect(ex - 1, y - 1, cw + 1, h + 2, WHITE);
        draw_rect(ex, y, cw - 1, h, BLACK);
    }
}

static void picked(void *user, const uint8_t *rgb) {
    asmdraw_state_t *s = user;
    if (!rgb)
        return; /* (cancelled: the picker put the colour back) */
    memcpy(s->custom[s->edit], rgb, 3);
    apply_custom(s);
    wm_invalidate(s->win);
}

static void edit_custom(asmdraw_state_t *s) {
    char t[24];
    snprintf(t, sizeof(t), "Custom colour %d", s->edit + 1);
    /* tried in its own palette entry: the picture shows it at once */
    dlg_color(s->win, t, s->custom[s->edit], FIRST_CUSTOM + s->edit, picked, s);
}

static void on_palette(control *c, int ev) {
    asmdraw_state_t *s = c->user;
    if (ev != EV_MOUSEDOWN && ev != EV_DBLCLICK)
        return;
    int n = swatches(s), cw = c->aw / n;
    int i = cw > 0 ? c->mx / cw : -1;
    if (i < 0 || i >= n)
        return;
    if (kb.shift)
        s->bg_color = swatch_color(i);
    else
        s->fg_color = swatch_color(i);
    if (i >= 16) { /* a custom one: Edit... changes it (double-click too) */
        s->edit = i - 16;
        if (ev == EV_DBLCLICK)
            edit_custom(s);
    }
    wm_invalidate(s->win);
}

static void on_edit_btn(control *c, int ev) {
    (void)ev;
    edit_custom(c->user);
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
    kfree(s->floating); /* (a selection belongs to the old picture) */
    s->floating = NULL;
    s->sel = s->moving = s->selecting = false;
    asmdraw_load(s, s->path);
    set_title(s);
    note_dir(s);
}

static void menu_new(void) {
    asmdraw_state_t *s = ui_active_state();
    if (!s)
        return;
    kfree(s->floating); /* (the selection goes with the picture) */
    s->floating = NULL;
    s->sel = s->moving = s->selecting = false;
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
    if (s->sel)
        put_down(s);
    const char *base = strrchr(s->path, '/');
    base = base ? base + 1 : (s->path[0] ? s->path : "Picture");
    print_doc_t doc = {PRINT_DOC_IMAGE, base,        NULL,        0,
                       s->canvas,       s->canvas_w, s->canvas_h, s->canvas_w};
    dlg_print(s->win, &doc);
}

static void menu_about(void) {
    dlg_message(NULL, "About ASMDraw",
                "ASMDraw v3.0\nPencil, eraser, fill, colour picking, spray, "
                "lines, rectangles and ellipses (filled too; Shift: "
                "straight, squares, circles), 6 brush sizes.\n"
                "Click a colour for the pen, Shift+click for the paper. On "
                "a 256-colour screen, 8 colours of your own: Edit...\n"
                ".PIC and .BMP pictures.");
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
    s->win->min_w = 270;
    s->win->min_h = 180;
    window *w = s->win;

    /* the screen's colours, and this window's own 8 (white at first) */
    A->palette256(s->pal, 256); /* (all of them: a copy keeps them) */
    s->many = A->screen_palette(s->pal, 0) >= 256;
    for (int k = 0; k < CUSTOM; k++) {
        char key[4] = {'c', (char)('0' + k), 0};
        int v = app_prefs_get_int("ASMDRAW", key, 0x3FFFF);
        s->custom[k][0] = (uint8_t)((v >> 12) & 63);
        s->custom[k][1] = (uint8_t)((v >> 6) & 63);
        s->custom[k][2] = (uint8_t)(v & 63);
    }
    apply_custom(s);
    s->seed = time_millis() | 1;

    int cx, cy, cw, ch;
    win_client_rect(w, &cx, &cy, &cw, &ch);
    int sy = ch - 11 - STRIP - 2; /* the colours' row (over the status) */
    for (int i = 0; i < NUM_TOOLS; i++) {
        int t = BAR_TOOLS[i];
        control *b = ctl_custom(w, 2 + (i % 2) * BTN, 2 + (i / 2) * BTN,
                                BTN - 1, BTN - 1, paint_button, on_button, 0);
        b->id = t;
        ctl_set_tooltip(b, TOOL_NAMES[t]);
        s->tools[t] = b;
    }
    static const char *const SIZE_TIPS[NUM_SIZES] = {
        "Brush: 1 pixel",  "Brush: 2 pixels", "Brush: 3 pixels",
        "Brush: 5 pixels", "Brush: 7 pixels", "Brush: 9 pixels"};
    int y0 = 2 + (NUM_TOOLS + 1) / 2 * BTN + 4;
    for (int i = 0; i < NUM_SIZES; i++) {
        control *b = ctl_custom(w, 2 + (i % 2) * BTN, y0 + (i / 2) * SBTN,
                                BTN - 1, SBTN - 1, paint_button, on_button, 0);
        b->id = SIZE_ID + i;
        ctl_set_tooltip(b, SIZE_TIPS[i]);
        s->sizes[i] = b;
    }
    s->canvas_ctl =
        ctl_custom(w, BAR, 2, cw - BAR - 2, sy - 4, paint_canvas, on_canvas,
                   CS_BORDER | CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);
    s->pens = ctl_custom(w, 2, sy, BAR - 6, STRIP, paint_pens, on_pens,
                         CS_ANCHOR_BOTTOM);
    ctl_set_tooltip(s->pens, "The pen over the paper: click to swap them");
    int edit_w = s->many ? 40 : 0;
    s->palette =
        ctl_custom(w, BAR, sy, cw - BAR - 2 - edit_w, STRIP, paint_palette,
                   on_palette, CS_STRETCH_W | CS_ANCHOR_BOTTOM);
    ctl_set_tooltip(s->palette,
                    s->many ? "Click: the pen's colour, Shift+click: the "
                              "paper's. The last 8 are your own"
                            : "Click: the pen's colour, Shift+click: the "
                              "paper's");
    if (s->many) {
        s->edit_btn = ctl_button(w, cw - 2 - 38, sy, 38, STRIP, "Edit...", 0,
                                 on_edit_btn);
        ctl_style_add(s->edit_btn,
                      CS_ANCHOR_RIGHT | CS_ANCHOR_BOTTOM | CS_NOFOCUS);
        ctl_set_tooltip(s->edit_btn, "Change the framed custom colour");
    }
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
    menu *e = window_add_menu(w, "Edit");
    menu_add_item(e, "Cut", menu_cut);
    menu_add_item(e, "Copy", menu_copy);
    menu_add_item(e, "Paste", menu_paste);
    menu_add_item(e, "Delete", menu_delete);
    menu_add_separator(e);
    menu_add_item(e, "Select all", menu_select_all);

    /* (a custom colour only where there are 256) */
    int pen = app_prefs_get_int("ASMDRAW", "pen", BLACK);
    int paper = app_prefs_get_int("ASMDRAW", "paper", WHITE);
    s->fg_color =
        (uint8_t)(pen >= 0 && pen < (s->many ? 256 : 16) ? pen : BLACK);
    s->bg_color =
        (uint8_t)(paper >= 0 && paper < (s->many ? 256 : 16) ? paper : WHITE);
    int tool = app_prefs_get_int("ASMDRAW", "tool", TOOL_PENCIL);
    int old = app_prefs_get_int("ASMDRAW", "brush", 0); /* (ASMDraw 2) */
    int size = app_prefs_get_int("ASMDRAW", "size",
                                 old == 2   ? 3
                                 : old == 1 ? 2
                                            : 0);
    s->before_pick = TOOL_PENCIL;
    select_tool(
        s,
        tool >= 0 && tool < NUM_TOOLS && tool != TOOL_PICK ? tool : TOOL_PENCIL,
        size >= 0 && size < NUM_SIZES ? size : 0);
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
    /* the four arrows over the selection: it can be dragged */
    control *cc = s->canvas_ctl;
    if (focus && s->tool == TOOL_SELECT &&
        (s->moving || (mouse.x >= cc->ax && mouse.y >= cc->ay &&
                       mouse.x < cc->ax + cc->aw && mouse.y < cc->ay + cc->ah &&
                       in_sel(s, mouse.x - cc->ax - 2, mouse.y - cc->ay - 2))))
        A->pointer(APP_POINTER_MOVE);
    if (s->drawing && s->tool == TOOL_SPRAY && mouse.left) { /* held still */
        spray(s, s->cur_x, s->cur_y);
        wm_invalidate(s->win);
    }
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
        app_prefs_set_int("ASMDRAW", "tool",
                          s->tool == TOOL_PICK ? s->before_pick : s->tool);
        app_prefs_set_int("ASMDRAW", "size", s->size);
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
    kfree(s->floating);
    s->floating = NULL;
}

APP_META("ASMDraw", asmdraw_state_t, false, APP_HEAP_PLUS_SCREEN | 128,
         asmdraw_init, asmdraw_on_frame, asmdraw_destroy);
