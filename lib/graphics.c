#include "lib/graphics.h"
#include "drivers/gpu.h"
#include "fonts/fonts.h"
#include "lib/core.h"
#include "lib/math.h"
#include "lib/memory.h"
#include "lib/string.h"

/* Clip rectangle (screen coords). Everything below honours it; by default
 * it is the whole screen. UI controls set it to their own bounds. */
static int clip_x0 = 0, clip_y0 = 0, clip_x1 = 1 << 15, clip_y1 = 1 << 15;

void gfx_set_clip(int x, int y, int w, int h) {
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w;
    clip_y1 = y + h;
}

void gfx_get_clip(int *x, int *y, int *w, int *h) {
    *x = clip_x0;
    *y = clip_y0;
    *w = clip_x1 - clip_x0;
    *h = clip_y1 - clip_y0;
}

void gfx_reset_clip(void) {
    clip_x0 = clip_y0 = 0;
    clip_x1 = clip_y1 = 1 << 15;
}

void draw_dot(int x, int y, unsigned char color) {
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1)
        return;
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT)
        return;
    BACKBUF[y * SCREEN_WIDTH + x] = color;
}

/* vertical run, clipped once */
static void vline(int x, int y0, int y1, unsigned char color) {
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    if (x < clip_x0 || x >= clip_x1 || x >= SCREEN_WIDTH)
        return;
    if (y0 < clip_y0)
        y0 = clip_y0;
    if (y1 >= clip_y1)
        y1 = clip_y1 - 1;
    if (y1 >= SCREEN_HEIGHT)
        y1 = SCREEN_HEIGHT - 1;
    unsigned char *p = BACKBUF + y0 * SCREEN_WIDTH + x;
    for (int y = y0; y <= y1; y++, p += SCREEN_WIDTH)
        *p = color;
}

void draw_line(int x0, int y0, int x1, int y1, unsigned char color) {
    /* bevels and borders are nearly all straight: no per-dot Bresenham */
    if (y0 == y1) {
        if (x0 > x1) {
            int t = x0;
            x0 = x1;
            x1 = t;
        }
        fill_rect(x0, y0, x1 - x0 + 1, 1, color);
        return;
    }
    if (x0 == x1) {
        vline(x0, y0, y1, color);
        return;
    }
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        draw_dot(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_rect(int x, int y, int w, int h, unsigned char color) {
    draw_line(x, y, x + w - 1, y, color);
    draw_line(x, y, x, y + h - 1, color);
    draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void fill_rect(int x, int y, int w, int h, unsigned char color) {
    if (w <= 0 || h <= 0)
        return;
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (x1 > SCREEN_WIDTH)
        x1 = SCREEN_WIDTH;
    if (y1 > SCREEN_HEIGHT)
        y1 = SCREEN_HEIGHT;
    if (x0 >= x1 || y0 >= y1)
        return;
    unsigned char *buf = BACKBUF;
    for (int row = y0; row < y1; row++)
        memset(buf + row * SCREEN_WIDTH + x0, color, x1 - x0);
}

/* A w x h block of pixels (row pitch `stride`) put at x, y: clipped once,
 * then copied a row at a time. Much faster than draw_dot per pixel. */
void draw_pixels(int x, int y, int w, int h, const uint8_t *src, int stride) {
    if (w <= 0 || h <= 0 || !src)
        return;
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (x1 > SCREEN_WIDTH)
        x1 = SCREEN_WIDTH;
    if (y1 > SCREEN_HEIGHT)
        y1 = SCREEN_HEIGHT;
    if (x0 >= x1 || y0 >= y1)
        return;
    const uint8_t *in = src + (y0 - y) * stride + (x0 - x);
    unsigned char *out = BACKBUF + y0 * SCREEN_WIDTH + x0;
    for (int row = y0; row < y1; row++) {
        memcpy(out, in, (uint32_t)(x1 - x0));
        in += stride;
        out += SCREEN_WIDTH;
    }
}

static const pfont_t *pfont(int size) {
    return size == FONT_SYSTEM                       ? &font_system
           : size == FONT_SMALL || size == FONT_TERM ? &font_small
                                                     : NULL;
}

static int glyph_index(char c) {
    unsigned char u = (unsigned char)c;
    return (u < 32 || u > 126) ? '?' - 32 : u - 32;
}

/* one proportional glyph; returns its advance */
static int draw_pglyph(int x, int y, char c, unsigned char color,
                       const pfont_t *f, int size) {
    int g = glyph_index(c);
    int w = f->width[g], h = f->h;
    if (size == FONT_TERM) /* centred in its fixed cell */
        x += (FONT_TERM_W - 1 - w) / 2;
    const uint8_t *rows = f->bits + g * h;
    bool inside = x >= clip_x0 && y >= clip_y0 && x + w <= clip_x1 &&
                  y + h <= clip_y1 && x >= 0 && y >= 0 &&
                  x + w <= SCREEN_WIDTH && y + h <= SCREEN_HEIGHT;
    for (int r = 0; r < h; r++) {
        uint8_t bits = rows[r];
        if (!bits)
            continue;
        if (inside) {
            unsigned char *out = BACKBUF + (y + r) * SCREEN_WIDTH + x;
            for (int col = 0; bits; col++, bits <<= 1)
                if (bits & 0x80)
                    out[col] = color;
        } else {
            for (int col = 0; bits; col++, bits <<= 1)
                if (bits & 0x80)
                    draw_dot(x + col, y + r, color);
        }
    }
    return w + 1;
}

/* advance of one character in a font */
static int char_advance(char c, int size) {
    if (size == FONT_TERM)
        return FONT_TERM_W;
    const pfont_t *f = pfont(size);
    if (f)
        return f->width[glyph_index(c)] + 1;
    return size == FONT_MONO8 ? 8 : 5;
}

int font_height(int size) {
    const pfont_t *f = pfont(size);
    return f ? f->h : size == FONT_MONO8 ? 8 : 6;
}

void draw_char(int x, int y, char c, unsigned char color, int size) {
    if (pfont(size)) {
        draw_pglyph(x, y, c, color, pfont(size), size);
        return;
    }
    if (y < 0 || y >= SCREEN_HEIGHT)
        return;
    if ((unsigned char)c < 32 || (unsigned char)c > 126)
        c = '?'; /* the fonts only cover printable ASCII */
    int offset, maxrow, maxcol;
    unsigned char startbit;
    if (size == 1) {
        offset = (c - 32) * 8;
        maxrow = 8;
        maxcol = 8;
        startbit = 0x80;
    } else {
        offset = (c - 32) * 6;
        maxrow = 6;
        maxcol = 4;
        startbit = 0x08;
    }
    const unsigned char *font = (size == 1) ? font1_data : font2_data;
    /* fast path: the whole glyph is inside the clip, so no per-dot checks */
    if (x >= clip_x0 && y >= clip_y0 && x + maxcol <= clip_x1 &&
        y + maxrow <= clip_y1 && x + maxcol <= SCREEN_WIDTH &&
        y + maxrow <= SCREEN_HEIGHT) {
        unsigned char *out = BACKBUF + y * SCREEN_WIDTH + x;
        for (int row = 0; row < maxrow; row++, out += SCREEN_WIDTH) {
            unsigned char bits = font[offset + row];
            for (int col = 0; bits && col < maxcol; col++)
                if (bits & (startbit >> col))
                    out[col] = color;
        }
        return;
    }
    for (int row = 0; row < maxrow; row++) {
        if (y + row < 0 || y + row >= SCREEN_HEIGHT)
            continue;
        unsigned char row_data =
            (size == 1) ? font1_data[offset + row] : font2_data[offset + row];
        for (int col = 0; col < maxcol; col++)
            if (row_data & (startbit >> col))
                draw_dot(x + col, y + row, color);
    }
}

void draw_string(int x, int y, const char *str, unsigned char color, int size) {
    const pfont_t *f = pfont(size);
    int cx = x;
    for (int i = 0; str[i]; i++) {
        if (cx >= SCREEN_WIDTH || cx >= clip_x1)
            break;
        int adv = char_advance(str[i], size);
        if (cx + adv > 0) {
            if (f)
                draw_pglyph(cx, y, str[i], color, f, size);
            else
                draw_char(cx, y, str[i], color, size);
        }
        cx += adv;
    }
}

void draw_text_n(int x, int y, const char *str, int n, unsigned char color,
                 int size) {
    int cx = x;
    for (int i = 0; i < n && str[i]; i++) {
        draw_char(cx, y, str[i], color, size);
        cx += char_advance(str[i], size);
    }
}

int text_width(const char *str, int size) {
    if (!pfont(size) || size == FONT_TERM)
        return (int)strlen(str) * char_advance('x', size);
    int w = 0;
    for (; *str; str++)
        w += char_advance(*str, size);
    return w;
}

int text_width_n(const char *str, int n, int size) {
    int w = 0;
    for (int i = 0; i < n && str[i]; i++)
        w += char_advance(str[i], size);
    return w;
}

/* how many leading characters of str fit in `width` pixels */
int text_fit(const char *str, int width, int size) {
    int w = 0, n = 0;
    for (; str[n]; n++) {
        int a = char_advance(str[n], size);
        if (w + a - 1 > width) /* the last column of gap needn't fit */
            break;
        w += a;
    }
    return n;
}

void blit(void) { gpu_blit(); }

void clear_screen(unsigned char color) {
    memset(BACKBUF, color,
           (uint32_t)(SCREEN_WIDTH * SCREEN_HEIGHT)); /* rep stosd */
}
