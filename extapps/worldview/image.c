// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* Pictures: GIF (the first frame, transparency), BMP (1 to 32 bits, not
 * compressed) and JPEG (baseline: what most sites and the image proxies
 * send; not progressive). Each is read a row at a time into a sink that
 * makes it the size it is shown at and gives it the screen's colours:
 * the nearest of the 248 (256-colour screens), or of the 16 with an
 * ordered dither. A JPEG shown at an eighth of its size or less is read
 * from its blocks' averages alone (no IDCT: fast). */

#include "extapps/worldview/wv.h"

// the screen's colours

static uint8_t s_pal[256][3]; /* 0..63 */
static int s_ncol = 16;
static uint8_t s_lut[4096]; /* 4 bits of each: the nearest, 0xFF unknown */

void img_palette_init(void) {
    int n = A->screen_palette(s_pal, 256);
    s_ncol = n > APP_COLOR_CUSTOM ? APP_COLOR_CUSTOM : n < 16 ? 16 : n;
    memset(s_lut, 0xFF, sizeof(s_lut));
}

static uint8_t nearest(int r, int g, int b) {
    int best = 0, bd = 0x7FFFFFFF;
    for (int k = 0; k < s_ncol; k++) {
        int dr = r - s_pal[k][0] * 4, dg = g - s_pal[k][1] * 4,
            db = b - s_pal[k][2] * 4;
        int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return (uint8_t)best;
}

uint8_t img_nearest(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    int i = (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4);
    if (s_lut[i] == 0xFF)
        s_lut[i] = nearest((r & 0xF0) | 8, (g & 0xF0) | 8, (b & 0xF0) | 8);
    return s_lut[i];
}

static const int8_t BAYER[16] = {0, 8,  2, 10, 12, 4, 14, 6,
                                 3, 11, 1, 9,  15, 7, 13, 5};

// the sink: rows in, the size shown, the screen's colours

typedef struct {
    int sw, sh, dw, dh;
    uint8_t *out;
    int next; /* the next row out */
    int amp;  /* dither strength */
} sink_t;

static void sink_row(sink_t *k, int sy, const uint8_t *rgb) {
    while (k->next < k->dh && k->next * k->sh / k->dh == sy) {
        int y = k->next++;
        uint8_t *o = k->out + y * k->dw;
        for (int x = 0; x < k->dw; x++) {
            const uint8_t *p = rgb + (x * k->sw / k->dw) * 3;
            int d = (BAYER[(y & 3) * 4 + (x & 3)] - 8) * k->amp / 8;
            o[x] = img_nearest(p[0] + d, p[1] + d, p[2] + d);
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
    int c = (int)(g->bits & ((1u << size) - 1));
    g->bits >>= size;
    g->nbits -= size;
    return c;
}

/* the first frame's colour indexes into idx (w x h) */
static bool gif_lzw(gif_bits_t *g, int min, uint8_t *idx, int w, int h,
                    bool interlace) {
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], stack[4097];
    int clear = 1 << min, eoi = clear + 1, avail = clear + 2;
    int size = min + 1, old = -1;
    uint8_t first = 0;
    int x = 0, y = 0, pass = 0, total = w * h, done = 0;
    static const int START[4] = {0, 4, 2, 1}, STEP[4] = {8, 8, 4, 2};
    for (int c = 0; c < clear; c++) {
        prefix[c] = 0xFFFF;
        suffix[c] = (uint8_t)c;
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
            idx[y * w + x] = stack[--sp];
            done++;
            if (++x == w) {
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

static bool gif_decode(const uint8_t *b, uint32_t n, sink_t *k, uint8_t bg,
                       const char **why) {
    gif_info_t gi;
    if (!gif_parse(b, n, &gi)) {
        *why = "a broken GIF";
        return false;
    }
    if ((uint32_t)gi.w * (uint32_t)gi.h > 640u * 480u) {
        *why = "a GIF too big to show";
        return false;
    }
    uint8_t *idx = (uint8_t *)kmalloc((uint32_t)(gi.w * gi.h));
    uint8_t *rgb = (uint8_t *)kmalloc((uint32_t)gi.w * 3);
    if (!idx || !rgb) {
        kfree(idx);
        kfree(rgb);
        *why = "not enough memory";
        return false;
    }
    memset(idx, gi.trans >= 0 ? gi.trans : 0, (uint32_t)(gi.w * gi.h));
    gif_bits_t g = {gi.data, b + n, 0, 0, 0};
    gif_lzw(&g, gi.min, idx, gi.w, gi.h, gi.interlace);
    const uint8_t *ct = gi.lct ? gi.lct : gi.gct;
    int ctn = gi.lct ? gi.lct_n : gi.gct_n;
    int br = s_pal[bg][0] * 4, bgg = s_pal[bg][1] * 4, bb = s_pal[bg][2] * 4;
    for (int y = 0; y < gi.h; y++) {
        for (int x = 0; x < gi.w; x++) {
            int c = idx[y * gi.w + x];
            uint8_t *o = rgb + x * 3;
            if (c == gi.trans || !ct || c >= ctn) {
                o[0] = (uint8_t)br, o[1] = (uint8_t)bgg, o[2] = (uint8_t)bb;
            } else {
                o[0] = ct[c * 3], o[1] = ct[c * 3 + 1], o[2] = ct[c * 3 + 2];
            }
        }
        sink_row(k, y, rgb);
    }
    kfree(idx);
    kfree(rgb);
    return true;
}

// BMP

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool bmp_parse(const uint8_t *b, uint32_t n, int *w, int *h, int *bpp,
                      bool *topdown, uint32_t *off, const uint8_t **pal,
                      int *palsz, int *pal_n) {
    if (n < 26 || b[0] != 'B' || b[1] != 'M')
        return false;
    uint32_t hs = le32(b + 14);
    *off = le32(b + 10);
    if (hs == 12) { /* (OS/2) */
        *w = le16(b + 18);
        *h = (int16_t)le16(b + 20);
        *bpp = le16(b + 24);
        *palsz = 3;
    } else if (hs >= 40 && n >= 54) {
        *w = (int32_t)le32(b + 18);
        *h = (int32_t)le32(b + 22);
        *bpp = le16(b + 28);
        uint32_t comp = le32(b + 30);
        if (comp != 0 && !(comp == 3 && (*bpp == 32 || *bpp == 16)))
            return false; /* (RLE: not here) */
        *palsz = 4;
        *pal_n = (int)le32(b + 46);
    } else {
        return false;
    }
    *topdown = *h < 0;
    if (*h < 0)
        *h = -*h;
    *pal = b + 14 + hs;
    if (*bpp <= 8 && (*pal_n <= 0 || *pal_n > (1 << *bpp)))
        *pal_n = 1 << *bpp;
    return *w > 0 && *h > 0 && *w <= 4096 && *h <= 4096 &&
           (*bpp == 1 || *bpp == 4 || *bpp == 8 || *bpp == 16 || *bpp == 24 ||
            *bpp == 32);
}

static bool bmp_decode(const uint8_t *b, uint32_t n, sink_t *k,
                       const char **why) {
    int w, h, bpp, palsz = 4, pal_n = 0;
    bool topdown;
    uint32_t off;
    const uint8_t *pal;
    if (!bmp_parse(b, n, &w, &h, &bpp, &topdown, &off, &pal, &palsz, &pal_n)) {
        *why = "a BMP it can't read";
        return false;
    }
    uint32_t stride = (uint32_t)((w * bpp + 31) / 32 * 4);
    if (off + stride * (uint32_t)h > n) {
        *why = "a BMP cut short";
        return false;
    }
    uint8_t *rgb = (uint8_t *)kmalloc((uint32_t)w * 3);
    if (!rgb) {
        *why = "not enough memory";
        return false;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *row =
            b + off + stride * (uint32_t)(topdown ? y : h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint8_t *o = rgb + x * 3;
            int c;
            if (bpp <= 8) {
                int per = 8 / bpp;
                c = (row[x / per] >> ((per - 1 - x % per) * bpp)) &
                    ((1 << bpp) - 1);
                const uint8_t *e = pal + c * palsz;
                if (c >= pal_n || e + 3 > b + n) {
                    o[0] = o[1] = o[2] = 0;
                } else {
                    o[0] = e[2], o[1] = e[1], o[2] = e[0];
                }
            } else if (bpp == 16) {
                uint16_t v = le16(row + x * 2); /* 5-5-5 */
                o[0] = (uint8_t)((v >> 10 & 31) << 3);
                o[1] = (uint8_t)((v >> 5 & 31) << 3);
                o[2] = (uint8_t)((v & 31) << 3);
            } else {
                const uint8_t *e = row + x * (bpp / 8);
                o[0] = e[2], o[1] = e[1], o[2] = e[0];
            }
        }
        sink_row(k, y, rgb);
    }
    kfree(rgb);
    return true;
}

// JPEG (baseline)

static const uint8_t ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* c(u) cos((2x+1)u pi/16) / 2, x 4096 */
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
                if (nx == 0x00) {
                    j->p += 2;
                } else {
                    b = 0; /* a marker: zeros until it is dealt with */
                }
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

/* a block: its 64 coefficients (dequantized, in order); false: broken */
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

/* the 8x8 pixels of a block (IDCT), into out (stride) */
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
            for (int c = 0; c < j->ncomp; c++) {
                j->c[c].id = s[6 + c * 3];
                j->c[c].hs = s[7 + c * 3] >> 4;
                j->c[c].vs = s[7 + c * 3] & 15;
                j->c[c].tq = s[8 + c * 3] & 3;
                if (j->c[c].hs < 1 || j->c[c].hs > 2 || j->c[c].vs < 1 ||
                    j->c[c].vs > 2)
                    return false;
                j->hmax = j->c[c].hs > j->hmax ? j->c[c].hs : j->hmax;
                j->vmax = j->c[c].vs > j->vmax ? j->c[c].vs : j->vmax;
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
                for (int c = 0; c < j->ncomp; c++)
                    if (j->c[c].id == id) {
                        j->c[c].td = s[2 + k * 2] >> 4 & 3;
                        j->c[c].ta = s[2 + k * 2] & 3;
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

static bool jpg_decode(const uint8_t *b, uint32_t n, sink_t *k, int dw, int dh,
                       const char **why) {
    jpg_t *j = (jpg_t *)kmalloc(sizeof(jpg_t));
    if (!j) {
        *why = "not enough memory";
        return false;
    }
    if (!jpg_headers(j, b, n)) {
        kfree(j);
        *why = "a JPEG it can't read";
        return false;
    }
    if (j->progressive) {
        kfree(j);
        *why = "a progressive JPEG (not shown)";
        return false;
    }
    /* an eighth of its size or less: each block's average is enough */
    bool dc_only = dw * 8 <= j->w && dh * 8 <= j->h;
    int bs = dc_only ? 1 : 8; /* a block's pixels across */
    int mx = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    int my = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    int out_w = dc_only ? (j->w + 7) / 8 : j->w;
    int out_h = dc_only ? (j->h + 7) / 8 : j->h;
    k->sw = out_w;
    k->sh = out_h;
    uint8_t *plane[3] = {NULL, NULL, NULL};
    int pw[3];
    bool ok = true;
    for (int c = 0; c < j->ncomp && ok; c++) {
        pw[c] = mx * j->c[c].hs * bs;
        plane[c] = (uint8_t *)kmalloc((uint32_t)(pw[c] * j->c[c].vs * bs));
        ok = plane[c] != NULL;
    }
    uint8_t *rgb = ok ? (uint8_t *)kmalloc((uint32_t)out_w * 3) : NULL;
    int co[64];
    if (!rgb) {
        *why = "not enough memory";
        ok = false;
    }
    int mcus = 0;
    for (int row = 0; row < my && ok; row++) {
        for (int col = 0; col < mx && ok; col++) {
            if (j->restart && mcus && mcus % j->restart == 0) {
                /* a restart: the bits realigned, past RSTn, DCs from 0 */
                j->nbits = 0;
                while (j->p + 1 < j->end &&
                       !(j->p[0] == 0xFF && j->p[1] >= 0xD0 && j->p[1] <= 0xD7))
                    j->p++;
                if (j->p + 1 < j->end)
                    j->p += 2;
                for (int c = 0; c < j->ncomp; c++)
                    j->c[c].dc = 0;
            }
            mcus++;
            for (int c = 0; c < j->ncomp && ok; c++)
                for (int by = 0; by < j->c[c].vs && ok; by++)
                    for (int bx = 0; bx < j->c[c].hs && ok; bx++) {
                        ok = jblock(j, c, co);
                        int px = (col * j->c[c].hs + bx) * bs, py = by * bs;
                        uint8_t *o = plane[c] + py * pw[c] + px;
                        if (dc_only)
                            *o = clamp8((co[0] >> 3) + 128); /* (DC / 8) */
                        else
                            idct(co, o, pw[c]);
                    }
        }
        /* the MCU row's pixel rows: to RGB, to the sink */
        int rows = j->vmax * bs;
        for (int r = 0; r < rows && ok; r++) {
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
            sink_row(k, y, rgb);
        }
    }
    for (int c = 0; c < 3; c++)
        kfree(plane[c]);
    kfree(rgb);
    kfree(j);
    if (!ok && !*why)
        *why = "a broken JPEG";
    return ok;
}

// what kind, how big, decoded

int img_kind(const uint8_t *b, uint32_t n) {
    if (n >= 6 && !memcmp(b, "GIF8", 4))
        return 'G';
    if (n >= 2 && b[0] == 'B' && b[1] == 'M')
        return 'B';
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return 'J';
    if (n >= 8 && !memcmp(b, "\x89PNG", 4))
        return 'P';
    return 0;
}

bool img_size(const uint8_t *b, uint32_t n, int *w, int *h) {
    switch (img_kind(b, n)) {
    case 'G': {
        gif_info_t gi;
        if (!gif_parse(b, n, &gi))
            return false;
        *w = gi.w;
        *h = gi.h;
        return true;
    }
    case 'B': {
        int bpp, palsz, pal_n = 0;
        bool td;
        uint32_t off;
        const uint8_t *pal;
        return bmp_parse(b, n, w, h, &bpp, &td, &off, &pal, &palsz, &pal_n);
    }
    case 'J': {
        jpg_t *j = (jpg_t *)kmalloc(sizeof(jpg_t));
        bool ok = j && jpg_headers(j, b, n);
        if (ok) {
            *w = j->w;
            *h = j->h;
        }
        kfree(j);
        return ok;
    }
    }
    return false;
}

uint8_t *img_decode(const uint8_t *b, uint32_t n, int dw, int dh, uint8_t bg,
                    const char **why) {
    *why = NULL;
    int kind = img_kind(b, n), sw, sh;
    if (kind == 'P') {
        *why = "a PNG (not shown)";
        return NULL;
    }
    if (!kind || !img_size(b, n, &sw, &sh)) {
        *why = "not a picture it knows";
        return NULL;
    }
    if (dw < 1 || dh < 1 || (uint32_t)(dw * dh) > WV_IMG_PIX_MAX) {
        *why = "too big";
        return NULL;
    }
    sink_t k = {sw, sh, dw, dh, NULL, 0, s_ncol > 16 ? 3 : 12};
    k.out = (uint8_t *)kmalloc((uint32_t)(dw * dh));
    if (!k.out) {
        *why = "not enough memory";
        return NULL;
    }
    memset(k.out, bg, (uint32_t)(dw * dh));
    bool ok = kind == 'G'   ? gif_decode(b, n, &k, bg, why)
              : kind == 'B' ? bmp_decode(b, n, &k, why)
                            : jpg_decode(b, n, &k, dw, dh, why);
    if (!ok) {
        kfree(k.out);
        return NULL;
    }
    return k.out;
}
