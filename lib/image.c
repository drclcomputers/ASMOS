// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* GIF, JPEG and BMP from memory (lib/image.h). Each is read a row at a
 * time into a sink that makes it the size asked for and gives it the
 * target's colours (through a lookup of 4 bits a channel, filled as it
 * goes). What a decoder works in comes from the target's allocator, and
 * goes back before it returns. */

#include "lib/image.h"
#include "lib/memory.h"
#include "lib/string.h"

typedef struct {
    const image_target_t *t;
    uint8_t *lut; /* 4096: R, G, B 4 bits each -> an index; 0xFF unknown */
    int amp;      /* dither strength */
    /* the sink */
    int sw, sh, dw, dh, stride;
    uint8_t *out;
    int next; /* the next row out */
} ctx_t;

static void *get(const ctx_t *c, uint32_t n) {
    return c->t->alloc ? c->t->alloc(n) : kmalloc(n);
}
static void put(const ctx_t *c, void *p) {
    if (!p)
        return;
    if (c->t->free)
        c->t->free(p);
    else
        kfree(p);
}

// colours

static uint8_t nearest(const ctx_t *c, int r, int g, int b) {
    int best = 0, bd = 0x7FFFFFFF;
    for (int k = 0; k < c->t->n; k++) {
        int dr = r - c->t->pal[k][0] * 4, dg = g - c->t->pal[k][1] * 4,
            db = b - c->t->pal[k][2] * 4;
        int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return (uint8_t)best;
}

static uint8_t colour(ctx_t *c, int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    int i = (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4);
    if (c->lut[i] == 0xFF)
        c->lut[i] = nearest(c, (r & 0xF0) | 8, (g & 0xF0) | 8, (b & 0xF0) | 8);
    return c->lut[i];
}

static const int8_t BAYER[16] = {0, 8,  2, 10, 12, 4, 14, 6,
                                 3, 11, 1, 9,  15, 7, 13, 5};

/* a source row (R, G, B): the output rows it makes */
static void sink_row(ctx_t *c, int sy, const uint8_t *rgb) {
    while (c->next < c->dh && c->next * c->sh / c->dh == sy) {
        int y = c->next++;
        uint8_t *o = c->out + y * c->stride;
        for (int x = 0; x < c->dw; x++) {
            const uint8_t *p = rgb + (x * c->sw / c->dw) * 3;
            int d = (BAYER[(y & 3) * 4 + (x & 3)] - 8) * c->amp / 8;
            o[x] = colour(c, p[0] + d, p[1] + d, p[2] + d);
        }
    }
}

// GIF

typedef struct {
    const uint8_t *p, *end;
    int block; /* bytes left in this sub-block */
    uint32_t bits;
    int nbits;
} gif_bits_t;

static int gif_byte(gif_bits_t *g) {
    if (!g->block) {
        if (g->p >= g->end || !*g->p)
            return -1;
        g->block = *g->p++;
    }
    if (g->p >= g->end)
        return -1;
    g->block--;
    return *g->p++;
}

static int gif_code(gif_bits_t *g, int size) {
    while (g->nbits < size) {
        int b = gif_byte(g);
        if (b < 0)
            return -1;
        g->bits |= (uint32_t)b << g->nbits;
        g->nbits += 8;
    }
    int code = (int)(g->bits & ((1u << size) - 1));
    g->bits >>= size;
    g->nbits -= size;
    return code;
}

/* a GIF row (its colour indexes) has come: into each output row made
 * from it, still as the GIF's indexes (made colours at the end): so no
 * copy of the whole picture is needed, only the output */
static void gif_row(ctx_t *c, int r, const uint8_t *row) {
    int y = (int)(((uint32_t)r * (uint32_t)c->dh + (uint32_t)c->sh - 1) /
                  (uint32_t)c->sh);
    for (; y < c->dh && y * c->sh / c->dh == r; y++) {
        uint8_t *o = c->out + y * c->stride;
        for (int x = 0; x < c->dw; x++)
            o[x] = row[x * c->sw / c->dw];
    }
}

/* the colour indexes, a row at a time (gif_row); row: w bytes; tables:
 * 4096 prefixes (16 bits), 4096 suffixes, a stack of 4097 */
static bool gif_lzw(ctx_t *c, gif_bits_t *g, int min, uint8_t *row, int w,
                    int h, bool interlace, uint16_t *prefix, uint8_t *suffix,
                    uint8_t *stack) {
    int clear = 1 << min, eoi = clear + 1, avail = clear + 2;
    int size = min + 1, old = -1;
    uint8_t first = 0;
    int x = 0, y = 0, pass = 0, total = w * h, done = 0;
    static const int START[4] = {0, 4, 2, 1}, STEP[4] = {8, 8, 4, 2};
    for (int k = 0; k < clear; k++) {
        prefix[k] = 0xFFFF;
        suffix[k] = (uint8_t)k;
    }
    while (done < total) {
        int code = gif_code(g, size);
        if (code < 0 || code == eoi)
            break;
        if (code == clear) {
            size = min + 1;
            avail = clear + 2;
            old = -1;
            continue;
        }
        int sp = 0, in = code;
        if (old < 0) {
            if (code >= clear)
                return false;
            stack[sp++] = (uint8_t)code;
            first = (uint8_t)code;
        } else {
            if (code > avail)
                return false;
            if (code == avail) {
                stack[sp++] = first;
                code = old;
            }
            while (code >= clear && sp < 4096) {
                stack[sp++] = suffix[code];
                code = prefix[code];
            }
            stack[sp++] = (uint8_t)code;
            first = (uint8_t)code;
            if (avail < 4096) {
                prefix[avail] = (uint16_t)old;
                suffix[avail] = first;
                avail++;
                if (avail == (1 << size) && size < 12)
                    size++;
            }
        }
        old = in;
        while (sp && done < total) {
            row[x] = stack[--sp];
            done++;
            if (++x == w) {
                gif_row(c, y, row);
                x = 0;
                if (interlace) {
                    y += STEP[pass];
                    while (y >= h && pass < 3)
                        y = START[++pass];
                } else {
                    y++;
                }
                if (y >= h)
                    done = total;
            }
        }
    }
    return true;
}

typedef struct {
    int w, h;
    const uint8_t *gct, *lct;
    int gct_n, lct_n;
    int trans; /* -1 none */
    bool interlace;
    int min;
    const uint8_t *data;
} gif_info_t;

static bool gif_parse(const uint8_t *b, uint32_t n, gif_info_t *gi) {
    memset(gi, 0, sizeof(*gi));
    gi->trans = -1;
    if (n < 13 || memcmp(b, "GIF8", 4))
        return false;
    const uint8_t *p = b + 13, *end = b + n;
    if (b[10] & 0x80) {
        gi->gct = p;
        gi->gct_n = 2 << (b[10] & 7);
        p += gi->gct_n * 3;
    }
    while (p < end) {
        if (*p == 0x21 && p + 2 < end) { /* an extension */
            if (p[1] == 0xF9 && p + 7 < end && (p[3] & 1))
                gi->trans = p[6];
            p += 2;
            while (p < end && *p)
                p += *p + 1;
            p++;
        } else if (*p == 0x2C && p + 10 <= end) { /* the picture */
            gi->w = p[5] | p[6] << 8;
            gi->h = p[7] | p[8] << 8;
            uint8_t fl = p[9];
            gi->interlace = (fl & 0x40) != 0;
            p += 10;
            if (fl & 0x80) {
                gi->lct = p;
                gi->lct_n = 2 << (fl & 7);
                p += gi->lct_n * 3;
            }
            if (p >= end)
                return false;
            gi->min = *p++;
            gi->data = p;
            return gi->w > 0 && gi->h > 0 && gi->min >= 2 && gi->min <= 11;
        } else {
            return false;
        }
    }
    return false;
}

static const char *gif_decode(ctx_t *c, const uint8_t *b, uint32_t n) {
    gif_info_t gi;
    if (!gif_parse(b, n, &gi))
        return "A broken GIF.";
    uint8_t *row = get(c, (uint32_t)gi.w);
    uint16_t *prefix = get(c, 4096 * 2);
    uint8_t *suffix = get(c, 4096), *stack = get(c, 4097);
    const char *why = NULL;
    if (!row || !prefix || !suffix || !stack) {
        why = "Not enough memory.";
    } else {
        /* (rows a broken GIF leaves out: see-through, else colour 0) */
        uint8_t none = (uint8_t)(gi.trans >= 0 ? gi.trans : 0);
        for (int y = 0; y < c->dh; y++)
            memset(c->out + y * c->stride, none, (uint32_t)c->dw);
        memset(row, none, (uint32_t)gi.w);
        gif_bits_t g = {gi.data, b + n, 0, 0, 0};
        gif_lzw(c, &g, gi.min, row, gi.w, gi.h, gi.interlace, prefix, suffix,
                stack);
        /* the GIF's indexes -> the screen's colours, in place */
        const uint8_t *ct = gi.lct ? gi.lct : gi.gct;
        int ctn = gi.lct ? gi.lct_n : gi.gct_n;
        const uint8_t *bg = c->t->bg;
        for (int y = 0; y < c->dh; y++) {
            uint8_t *o = c->out + y * c->stride;
            for (int x = 0; x < c->dw; x++) {
                int k = o[x];
                const uint8_t *p =
                    k == gi.trans || !ct || k >= ctn ? bg : ct + k * 3;
                int d = (BAYER[(y & 3) * 4 + (x & 3)] - 8) * c->amp / 8;
                o[x] = colour(c, p[0] + d, p[1] + d, p[2] + d);
            }
        }
    }
    put(c, stack);
    put(c, suffix);
    put(c, prefix);
    put(c, row);
    return why;
}

// BMP (not compressed)

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct {
    int w, h, bpp, palsz, pal_n;
    bool topdown;
    uint32_t off;
    const uint8_t *pal;
} bmp_mem_t;

static bool bmp_parse(const uint8_t *b, uint32_t n, bmp_mem_t *m) {
    memset(m, 0, sizeof(*m));
    if (n < 26 || b[0] != 'B' || b[1] != 'M')
        return false;
    uint32_t hs = le32(b + 14);
    m->off = le32(b + 10);
    if (hs == 12) { /* (OS/2) */
        m->w = le16(b + 18);
        m->h = (int16_t)le16(b + 20);
        m->bpp = le16(b + 24);
        m->palsz = 3;
    } else if (hs >= 40 && n >= 54) {
        m->w = (int32_t)le32(b + 18);
        m->h = (int32_t)le32(b + 22);
        m->bpp = le16(b + 28);
        uint32_t comp = le32(b + 30);
        if (comp != 0 && !(comp == 3 && (m->bpp == 32 || m->bpp == 16)))
            return false; /* (RLE: lib/bmp.h, from a file) */
        m->palsz = 4;
        m->pal_n = (int)le32(b + 46);
    } else {
        return false;
    }
    m->topdown = m->h < 0;
    if (m->h < 0)
        m->h = -m->h;
    m->pal = b + 14 + hs;
    if (m->bpp <= 8 && (m->pal_n <= 0 || m->pal_n > (1 << m->bpp)))
        m->pal_n = 1 << m->bpp;
    return m->w > 0 && m->h > 0 && m->w <= 8192 && m->h <= 8192 &&
           (m->bpp == 1 || m->bpp == 4 || m->bpp == 8 || m->bpp == 16 ||
            m->bpp == 24 || m->bpp == 32);
}

static const char *bmp_mem_decode(ctx_t *c, const uint8_t *b, uint32_t n) {
    bmp_mem_t m;
    if (!bmp_parse(b, n, &m))
        return "A BMP it can't read from memory.";
    uint32_t stride = (uint32_t)((m.w * m.bpp + 31) / 32 * 4);
    if (m.off + stride * (uint32_t)m.h > n)
        return "A BMP cut short.";
    uint8_t *rgb = get(c, (uint32_t)m.w * 3);
    if (!rgb)
        return "Not enough memory.";
    for (int y = 0; y < m.h; y++) {
        const uint8_t *row =
            b + m.off + stride * (uint32_t)(m.topdown ? y : m.h - 1 - y);
        for (int x = 0; x < m.w; x++) {
            uint8_t *o = rgb + x * 3;
            if (m.bpp <= 8) {
                int per = 8 / m.bpp;
                int k = (row[x / per] >> ((per - 1 - x % per) * m.bpp)) &
                        ((1 << m.bpp) - 1);
                const uint8_t *e = m.pal + k * m.palsz;
                if (k >= m.pal_n || e + 3 > b + n)
                    o[0] = o[1] = o[2] = 0;
                else
                    o[0] = e[2], o[1] = e[1], o[2] = e[0];
            } else if (m.bpp == 16) {
                uint16_t v = le16(row + x * 2); /* 5-5-5 */
                o[0] = (uint8_t)((v >> 10 & 31) << 3);
                o[1] = (uint8_t)((v >> 5 & 31) << 3);
                o[2] = (uint8_t)((v & 31) << 3);
            } else {
                const uint8_t *e = row + x * (m.bpp / 8);
                o[0] = e[2], o[1] = e[1], o[2] = e[0];
            }
        }
        sink_row(c, y, rgb);
    }
    put(c, rgb);
    return NULL;
}

// JPEG (baseline)

static const uint8_t ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* c(u) cos((2x+1)u pi/16) / 2, x 4096 (no floating point: a 386 may have
 * no FPU) */
static const int16_t COS[8][8] = {
    {1448, 1448, 1448, 1448, 1448, 1448, 1448, 1448},
    {2009, 1703, 1138, 400, -400, -1138, -1703, -2009},
    {1892, 784, -784, -1892, -1892, -784, 784, 1892},
    {1703, -400, -2009, -1138, 1138, 2009, 400, -1703},
    {1448, -1448, -1448, 1448, 1448, -1448, -1448, 1448},
    {1138, -2009, 400, 1703, -1703, -400, 2009, -1138},
    {784, -1892, 1892, -784, -784, 1892, -1892, 784},
    {400, -1138, 1703, -2009, 2009, -1703, 1138, -400}};

typedef struct {
    uint8_t vals[256];
    int maxcode[18], valptr[17], mincode[17];
    bool ok;
} huff_t;

typedef struct {
    const uint8_t *p, *end;
    uint16_t qt[4][64];
    huff_t ht[2][4]; /* [DC, AC][table] */
    int w, h, ncomp, hmax, vmax, restart;
    struct {
        int id, hs, vs, tq, td, ta, dc;
    } c[3];
    uint32_t bits;
    int nbits;
    bool progressive;
} jpg_t;

static void huff_build(huff_t *t, const uint8_t *counts, const uint8_t *vals,
                       int total) {
    memcpy(t->vals, vals, (uint32_t)(total > 256 ? 256 : total));
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        code += counts[l - 1];
        k += counts[l - 1];
        t->maxcode[l] = counts[l - 1] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    t->ok = true;
}

static int jbit(jpg_t *j) {
    if (!j->nbits) {
        int b = 0;
        if (j->p < j->end) {
            b = *j->p;
            if (b == 0xFF) {
                uint8_t nx = j->p + 1 < j->end ? j->p[1] : 0;
                if (nx == 0x00)
                    j->p += 2;
                else
                    b = 0; /* a marker: zeros until it is dealt with */
            } else {
                j->p++;
            }
        }
        j->bits = (uint32_t)b;
        j->nbits = 8;
    }
    j->nbits--;
    return (int)(j->bits >> j->nbits) & 1;
}

static int jbits(jpg_t *j, int n) {
    int v = 0;
    while (n--)
        v = v << 1 | jbit(j);
    return v;
}

static int jdecode(jpg_t *j, const huff_t *t) {
    int code = jbit(j), l = 1;
    while (l <= 16 && code > t->maxcode[l]) {
        code = code << 1 | jbit(j);
        l++;
    }
    if (l > 16)
        return 0;
    int k = t->valptr[l] + code - t->mincode[l];
    return k >= 0 && k < 256 ? t->vals[k] : 0;
}

static int extend(int v, int n) {
    return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

/* a block's 64 coefficients (dequantized, in order); false: broken */
static bool jblock(jpg_t *j, int ci, int *co) {
    const huff_t *dc = &j->ht[0][j->c[ci].td], *ac = &j->ht[1][j->c[ci].ta];
    if (!dc->ok || !ac->ok)
        return false;
    const uint16_t *q = j->qt[j->c[ci].tq];
    memset(co, 0, 64 * sizeof(int));
    int s = jdecode(j, dc);
    j->c[ci].dc += s ? extend(jbits(j, s), s) : 0;
    co[0] = j->c[ci].dc * q[0];
    for (int k = 1; k < 64;) {
        int rs = jdecode(j, ac), r = rs >> 4;
        s = rs & 15;
        if (!s) {
            if (r != 15)
                break; /* the rest are 0 */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            break;
        co[ZIGZAG[k]] = extend(jbits(j, s), s) * q[k];
        k++;
    }
    return true;
}

static uint8_t clamp8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* a block's 8x8 pixels (IDCT, in two passes) into out (stride) */
static void idct(const int *co, uint8_t *out, int stride) {
    int t[64];
    for (int v = 0; v < 8; v++) { /* rows: u -> x */
        const int *f = co + v * 8;
        bool flat = !(f[1] | f[2] | f[3] | f[4] | f[5] | f[6] | f[7]);
        for (int x = 0; x < 8; x++) {
            if (flat) {
                t[v * 8 + x] = (f[0] * 1448) >> 9;
                continue;
            }
            int s = 0;
            for (int u = 0; u < 8; u++)
                s += COS[u][x] * f[u];
            t[v * 8 + x] = s >> 9; /* (3 bits kept for the next pass) */
        }
    }
    for (int x = 0; x < 8; x++) /* columns: v -> y */
        for (int y = 0; y < 8; y++) {
            int s = 0;
            for (int v = 0; v < 8; v++)
                s += COS[v][y] * t[v * 8 + x];
            out[y * stride + x] = clamp8((s >> 15) + 128);
        }
}

static bool jpg_headers(jpg_t *j, const uint8_t *b, uint32_t n) {
    memset(j, 0, sizeof(*j));
    if (n < 4 || b[0] != 0xFF || b[1] != 0xD8)
        return false;
    const uint8_t *p = b + 2, *end = b + n;
    while (p + 4 <= end) {
        if (p[0] != 0xFF) {
            p++;
            continue;
        }
        uint8_t m = p[1];
        if (m == 0xFF) {
            p++;
            continue;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            p += 2;
            continue;
        }
        int len = p[2] << 8 | p[3];
        const uint8_t *s = p + 4, *e = p + 2 + len;
        if (e > end)
            return false;
        if (m == 0xDB) { /* quantization tables */
            while (s < e) {
                int pq = *s >> 4, tq = *s & 3;
                s++;
                for (int k = 0; k < 64 && s < e; k++) {
                    j->qt[tq][k] = pq ? (uint16_t)(s[0] << 8 | s[1]) : *s;
                    s += pq ? 2 : 1;
                }
            }
        } else if (m == 0xC4) { /* Huffman tables */
            while (s + 17 <= e) {
                int tc = *s >> 4, th = *s & 3, total = 0;
                for (int k = 0; k < 16; k++)
                    total += s[1 + k];
                if (tc > 1 || s + 17 + total > e)
                    return false;
                huff_build(&j->ht[tc][th], s + 1, s + 17, total);
                s += 17 + total;
            }
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) { /* the frame */
            j->progressive = m == 0xC2;
            j->h = s[1] << 8 | s[2];
            j->w = s[3] << 8 | s[4];
            j->ncomp = s[5];
            if (j->ncomp != 1 && j->ncomp != 3)
                return false;
            for (int k = 0; k < j->ncomp; k++) {
                j->c[k].id = s[6 + k * 3];
                j->c[k].hs = s[7 + k * 3] >> 4;
                j->c[k].vs = s[7 + k * 3] & 15;
                j->c[k].tq = s[8 + k * 3] & 3;
                if (j->c[k].hs < 1 || j->c[k].hs > 2 || j->c[k].vs < 1 ||
                    j->c[k].vs > 2)
                    return false;
                j->hmax = j->c[k].hs > j->hmax ? j->c[k].hs : j->hmax;
                j->vmax = j->c[k].vs > j->vmax ? j->c[k].vs : j->vmax;
            }
        } else if (m == 0xC3 ||
                   (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
            return false; /* (lossless, arithmetic...) */
        } else if (m == 0xDD) {
            j->restart = s[0] << 8 | s[1];
        } else if (m == 0xDA) { /* the scan */
            int ns = s[0];
            for (int k = 0; k < ns; k++) {
                int id = s[1 + k * 2];
                for (int q = 0; q < j->ncomp; q++)
                    if (j->c[q].id == id) {
                        j->c[q].td = s[2 + k * 2] >> 4 & 3;
                        j->c[q].ta = s[2 + k * 2] & 3;
                    }
            }
            j->p = e;
            j->end = end;
            return j->w > 0 && j->h > 0;
        } else if (m == 0xD9) {
            return false;
        }
        p = e;
    }
    return false;
}

static const char *jpg_decode(ctx_t *c, const uint8_t *b, uint32_t n) {
    jpg_t *j = get(c, sizeof(jpg_t));
    if (!j)
        return "Not enough memory.";
    if (!jpg_headers(j, b, n)) {
        put(c, j);
        return "A JPEG it can't read.";
    }
    if (j->progressive) {
        put(c, j);
        return "A progressive JPEG: not shown.";
    }
    /* an eighth of its size or less: each block's average is enough */
    bool dc_only = c->dw * 8 <= j->w && c->dh * 8 <= j->h;
    int bs = dc_only ? 1 : 8; /* a block's pixels across */
    int mx = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    int my = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    int out_w = dc_only ? (j->w + 7) / 8 : j->w;
    int out_h = dc_only ? (j->h + 7) / 8 : j->h;
    c->sw = out_w;
    c->sh = out_h;
    uint8_t *plane[3] = {NULL, NULL, NULL};
    int pw[3] = {0, 0, 0};
    const char *why = NULL;
    for (int k = 0; k < j->ncomp && !why; k++) {
        pw[k] = mx * j->c[k].hs * bs;
        plane[k] = get(c, (uint32_t)(pw[k] * j->c[k].vs * bs));
        why = plane[k] ? NULL : "Not enough memory.";
    }
    uint8_t *rgb = why ? NULL : get(c, (uint32_t)out_w * 3);
    int *co = why || !rgb ? NULL : get(c, 64 * sizeof(int));
    if (!why && (!rgb || !co))
        why = "Not enough memory.";
    int mcus = 0;
    for (int row = 0; row < my && !why; row++) {
        for (int col = 0; col < mx && !why; col++) {
            if (j->restart && mcus && mcus % j->restart == 0) {
                /* a restart: the bits realigned, past RSTn, DCs from 0 */
                j->nbits = 0;
                while (j->p + 1 < j->end &&
                       !(j->p[0] == 0xFF && j->p[1] >= 0xD0 && j->p[1] <= 0xD7))
                    j->p++;
                if (j->p + 1 < j->end)
                    j->p += 2;
                for (int k = 0; k < j->ncomp; k++)
                    j->c[k].dc = 0;
            }
            mcus++;
            for (int k = 0; k < j->ncomp && !why; k++)
                for (int by = 0; by < j->c[k].vs && !why; by++)
                    for (int bx = 0; bx < j->c[k].hs && !why; bx++) {
                        if (!jblock(j, k, co)) {
                            why = "A broken JPEG.";
                            break;
                        }
                        int px = (col * j->c[k].hs + bx) * bs, py = by * bs;
                        uint8_t *o = plane[k] + py * pw[k] + px;
                        if (dc_only)
                            *o = clamp8((co[0] >> 3) + 128); /* (DC / 8) */
                        else
                            idct(co, o, pw[k]);
                    }
        }
        /* the MCU row's pixel rows: to RGB, to the sink */
        int rows = j->vmax * bs;
        for (int r = 0; r < rows && !why; r++) {
            int y = row * rows + r;
            if (y >= out_h)
                break;
            for (int x = 0; x < out_w; x++) {
                int Y = plane[0][(r * j->c[0].vs / j->vmax) * pw[0] +
                                 x * j->c[0].hs / j->hmax];
                uint8_t *o = rgb + x * 3;
                if (j->ncomp == 1) {
                    o[0] = o[1] = o[2] = (uint8_t)Y;
                    continue;
                }
                int cb = plane[1][(r * j->c[1].vs / j->vmax) * pw[1] +
                                  x * j->c[1].hs / j->hmax] -
                         128;
                int cr = plane[2][(r * j->c[2].vs / j->vmax) * pw[2] +
                                  x * j->c[2].hs / j->hmax] -
                         128;
                o[0] = clamp8(Y + ((359 * cr) >> 8));
                o[1] = clamp8(Y - ((88 * cb + 183 * cr) >> 8));
                o[2] = clamp8(Y + ((454 * cb) >> 8));
            }
            sink_row(c, y, rgb);
        }
    }
    put(c, co);
    put(c, rgb);
    for (int k = 2; k >= 0; k--)
        put(c, plane[k]);
    put(c, j);
    return why;
}

// what kind, how big, decoded

int image_kind(const uint8_t *b, uint32_t n) {
    if (n >= 6 && !memcmp(b, "GIF8", 4))
        return IMAGE_GIF;
    if (n >= 2 && b[0] == 'B' && b[1] == 'M')
        return IMAGE_BMP;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return IMAGE_JPEG;
    if (n >= 8 && !memcmp(b, "\x89PNG", 4))
        return IMAGE_PNG;
    return 0;
}

const char *image_info(const uint8_t *b, uint32_t n, int *w, int *h) {
    switch (image_kind(b, n)) {
    case IMAGE_GIF: {
        gif_info_t gi;
        if (!gif_parse(b, n, &gi))
            return "A broken GIF.";
        *w = gi.w;
        *h = gi.h;
        return NULL;
    }
    case IMAGE_BMP: {
        bmp_mem_t m;
        if (!bmp_parse(b, n, &m))
            return "A BMP it can't read from memory.";
        *w = m.w;
        *h = m.h;
        return NULL;
    }
    case IMAGE_JPEG: {
        /* the frame's size: SOF0..2, without reading the rest */
        const uint8_t *p = b + 2, *end = b + n;
        while (p + 9 <= end) {
            if (p[0] != 0xFF) {
                p++;
                continue;
            }
            uint8_t m = p[1];
            if (m == 0xFF || m == 0xD8 || (m >= 0xD0 && m <= 0xD7) ||
                m == 0x01) {
                p += m == 0xFF ? 1 : 2;
                continue;
            }
            if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
                *h = p[5] << 8 | p[6];
                *w = p[7] << 8 | p[8];
                return *w && *h ? NULL : "A JPEG it can't read.";
            }
            if (m == 0xDA || m == 0xD9)
                break;
            p += 2 + (p[2] << 8 | p[3]);
        }
        return "A JPEG it can't read.";
    }
    case IMAGE_PNG:
        return "A PNG: not shown.";
    }
    return "Not a picture it knows.";
}

const char *image_decode(const uint8_t *b, uint32_t n, uint8_t *out, int dw,
                         int dh, int stride, const image_target_t *t) {
    int sw, sh;
    const char *why = image_info(b, n, &sw, &sh);
    if (why)
        return why;
    if (dw < 1 || dh < 1 || !out || !t || !t->pal || t->n < 1)
        return "Nothing to decode into.";
    ctx_t c = {
        t,   NULL, t->dither ? (t->n > 16 ? 3 : 12) : 0, sw, sh, dw, dh, stride,
        out, 0};
    c.lut = get(&c, 4096);
    if (!c.lut)
        return "Not enough memory.";
    memset(c.lut, 0xFF, 4096);
    /* (the background first: rows a broken picture leaves out) */
    uint8_t bgi = colour(&c, t->bg[0], t->bg[1], t->bg[2]);
    for (int y = 0; y < dh; y++)
        memset(out + y * stride, bgi, (uint32_t)dw);
    switch (image_kind(b, n)) {
    case IMAGE_GIF:
        why = gif_decode(&c, b, n);
        break;
    case IMAGE_BMP:
        why = bmp_mem_decode(&c, b, n);
        break;
    default:
        why = jpg_decode(&c, b, n);
    }
    put(&c, c.lut);
    return why;
}
