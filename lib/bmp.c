// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "lib/bmp.h"

#include "fs/fs.h"
#include "lib/mem.h"
#include "lib/string.h"

#define BUF 4096 /* bytes read from the file at a time */
#define CUBE (32 * 32 * 32)

/* part of a file: bytes read (0 past its end), -1 if it can't be read */
static int read_at(const char *path, uint32_t off, void *buf, int max) {
    fat_file_t f;
    if (!path || !buf || max < 0 || !fs_open(path, &f))
        return -1;
    int n = 0;
    if (off < f.entry.file_size && fs_seek(&f, off))
        n = fs_read(&f, buf, max);
    fs_close(&f);
    return n;
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

// the header

/* a bit mask: where it starts and how many bits */
static void mask_shape(uint32_t m, int *shift, int *bits) {
    int s = 0, b = 0;
    while (m && !(m & 1)) {
        m >>= 1;
        s++;
    }
    while (m & 1) {
        m >>= 1;
        b++;
    }
    *shift = s;
    *bits = b;
}

const char *bmp_open(const char *path, bmp_info_t *bi) {
    uint8_t h[70];
    memset(bi, 0, sizeof(*bi));
    memset(h, 0, sizeof(h));
    int got = read_at(path, 0, h, sizeof(h));
    if (got < 0)
        return "Can't read the file.";
    if (got < 26 || h[0] != 'B' || h[1] != 'M')
        return "Not a BMP picture.";
    bi->data_off = u32(h + 10);
    uint32_t hs = u32(h + 14); /* the info header's size */
    int planes;
    int32_t height;
    if (hs == 12) { /* OS/2 1.x */
        bi->w = u16(h + 18);
        height = (int16_t)u16(h + 20);
        planes = u16(h + 22);
        bi->bpp = u16(h + 24);
    } else if (hs >= 40 && got >= 54) {
        bi->w = (int32_t)u32(h + 18);
        height = (int32_t)u32(h + 22);
        planes = u16(h + 26);
        bi->bpp = u16(h + 28);
        bi->compression = u32(h + 30);
        bi->colors = (int)u32(h + 46);
    } else {
        return "Not a BMP picture.";
    }
    (void)planes;
    bi->top_down = height < 0;
    bi->h = height < 0 ? -height : height;
    if (bi->w < 1 || bi->h < 1)
        return "The picture is empty.";
    if (bi->w > BMP_MAX_SIDE || bi->h > BMP_MAX_SIDE)
        return "The picture is too big (more than 8192 pixels a side).";
    int b = bi->bpp;
    if (b != 1 && b != 4 && b != 8 && b != 16 && b != 24 && b != 32)
        return "Unsupported BMP: only 1, 4, 8, 16, 24 and 32 bits a pixel.";
    uint32_t c = bi->compression;
    bool ok = c == 0 || (c == 1 && b == 8) || (c == 2 && b == 4) ||
              (c == 3 && (b == 16 || b == 32));
    if (!ok || (bi->top_down && (c == 1 || c == 2)))
        return "Unsupported BMP compression.";
    bi->row_bytes = (uint32_t)(((bi->w * b + 31) / 32) * 4);

    /* colours in the pixels: the masks (BI_BITFIELDS after a 40-byte
     * header, or inside a bigger one: at 54 either way) */
    if (b >= 16) {
        if (c == 3) {
            uint8_t m[12];
            if (read_at(path, 54, m, 12) < 12)
                return "The picture is damaged.";
            for (int k = 0; k < 3; k++)
                bi->mask[k] = u32(m + 4 * k);
        } else if (b == 16) {
            bi->mask[0] = 0x7C00, bi->mask[1] = 0x03E0, bi->mask[2] = 0x001F;
        } else {
            bi->mask[0] = 0xFF0000, bi->mask[1] = 0xFF00, bi->mask[2] = 0xFF;
        }
        for (int k = 0; k < 3; k++)
            mask_shape(bi->mask[k], &bi->shift[k], &bi->bits[k]);
        bi->colors = 0;
        return NULL;
    }

    /* a palette: after the header (and the masks of a 40-byte one) */
    int entry = hs == 12 ? 3 : 4;
    uint32_t pal_off = 14 + hs + (hs == 40 && c == 3 ? 12 : 0);
    int n = bi->colors > 0 && bi->colors <= (1 << b) ? bi->colors : 1 << b;
    if (bi->data_off > pal_off &&
        (int)((bi->data_off - pal_off) / (uint32_t)entry) < n)
        n = (int)((bi->data_off - pal_off) / (uint32_t)entry);
    uint8_t raw[256 * 4];
    int rb = read_at(path, pal_off, raw, n * entry);
    if (rb < 0)
        rb = 0;
    n = rb / entry < n ? rb / entry : n;
    if (n < 1)
        return "The picture has no colours.";
    for (int i = 0; i < n; i++) {
        bi->pal[i][0] = raw[i * entry + 2];
        bi->pal[i][1] = raw[i * entry + 1];
        bi->pal[i][2] = raw[i * entry];
    }
    bi->colors = n;
    return NULL;
}

// reading

struct bmp_reader {
    bmp_info_t bi;
    char path[FS_PATH_SZ];
    bmp_target_t t;
    uint8_t p8[256][3]; /* the target's colours as 8 bits */
    uint8_t lut[256];   /* the picture's palette index -> the target's */
    uint8_t *cube;      /* 5 bits a channel -> nearest + 1 (0: not yet) */
    int16_t exact[512]; /* a target colour -> its index (hashed; -1 none) */
    int16_t *err;       /* dithering: two rows of (w + 2) * 3 errors */
    uint8_t *row;       /* a row of palette indexes, or of R G B */
    uint8_t *buf;       /* the file, BUF bytes from buf_at */
    uint32_t buf_at, pos;
    int buf_len;
    uint8_t *out;
    int out_w, out_h, stride;
    int done;                 /* rows read */
    bool rle_end, short_read; /* short_read: the file ended too soon */
    int rle_x, rle_blank;     /* RLE: next row starts at x; rows left blank */
    const char *error;
};

/* the next byte of the file, -1 at its end */
static int next_byte(bmp_reader_t *r) {
    if (r->pos < r->buf_at || r->pos >= r->buf_at + (uint32_t)r->buf_len) {
        r->buf_at = r->pos;
        r->buf_len = read_at(r->path, r->pos, r->buf, BUF);
        if (r->buf_len <= 0) {
            r->buf_len = 0;
            r->short_read = true;
            return -1;
        }
    }
    return r->buf[r->pos++ - r->buf_at];
}

static int nearest(const bmp_reader_t *r, int cr, int cg, int cb) {
    int best = 0, bd = 0x7FFFFFFF;
    for (int i = 0; i < r->t.n; i++) {
        int dr = cr - r->p8[i][0], dg = cg - r->p8[i][1], db = cb - r->p8[i][2];
        int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
        if (d < bd) {
            bd = d;
            best = i;
            if (!d)
                break;
        }
    }
    return best;
}

static int hash(int cr, int cg, int cb) {
    return (cr * 7 + cg * 31 + cb * 131) & 511;
}

/* the screen's own colours exactly, then the nearest through the cube
 * (each cell worked out once, from its middle) */
static int lookup(bmp_reader_t *r, int cr, int cg, int cb) {
    for (int h = hash(cr, cg, cb); r->exact[h] >= 0; h = (h + 1) & 511) {
        const uint8_t *p = r->p8[r->exact[h]];
        if (p[0] == cr && p[1] == cg && p[2] == cb)
            return r->exact[h];
    }
    int k = (cr >> 3) << 10 | (cg >> 3) << 5 | cb >> 3;
    if (!r->cube[k])
        r->cube[k] = (uint8_t)(1 + nearest(r, (cr & ~7) + 4, (cg & ~7) + 4,
                                           (cb & ~7) + 4));
    return r->cube[k] - 1;
}

/* the work memory: the reader, then its buffers (4-byte aligned) */
#define A4(n) (((n) + 3u) & ~3u)
static uint32_t row_size(const bmp_info_t *bi) {
    return A4((uint32_t)bi->w * (bi->bpp <= 8 ? 1u : 3u) + 8);
}
static bool needs_cube(const bmp_info_t *bi, const bmp_target_t *t) {
    return bi->bpp > 8 || t->dither;
}
static uint32_t err_size(const bmp_info_t *bi, const bmp_target_t *t) {
    return t->dither ? A4((uint32_t)(bi->w + 2) * 3 * 2 * 2) : 0;
}

uint32_t bmp_work_size(const bmp_info_t *bi, const bmp_target_t *t) {
    return A4(sizeof(bmp_reader_t)) + row_size(bi) + BUF +
           (needs_cube(bi, t) ? CUBE : 0) + err_size(bi, t);
}

bmp_reader_t *bmp_begin(const char *path, const bmp_info_t *bi,
                        const bmp_target_t *t, uint8_t *out, int out_w,
                        int out_h, int stride, void *work) {
    if (!work || !bi || !t || !out || bi->w < 1 || bi->w > BMP_MAX_SIDE)
        return NULL;
    bmp_reader_t *r = (bmp_reader_t *)work;
    memset(r, 0, sizeof(*r));
    r->bi = *bi;
    strncpy(r->path, path, sizeof(r->path) - 1);
    r->t = *t;
    if (r->t.n < 1 || r->t.n > 256)
        r->t.n = 16;
    for (int i = 0;
         i < 256 && i < (r->t.n > r->t.keep_n ? r->t.n : r->t.keep_n); i++)
        for (int k = 0; k < 3; k++)
            r->p8[i][k] = bmp_8bit(t->pal[i][k]);
    int w = bi->w;
    bool palette = bi->bpp <= 8;
    uint8_t *m = (uint8_t *)work + A4(sizeof(bmp_reader_t));
    r->row = m;
    m += row_size(bi);
    r->buf = m;
    m += BUF;
    if (needs_cube(bi, t)) {
        r->cube = m;
        m += CUBE;
    }
    if (t->dither)
        r->err = (int16_t *)m;
    if (r->cube)
        memset(r->cube, 0, CUBE);
    for (int i = 0; i < 512; i++)
        r->exact[i] = -1;
    for (int i = 0; i < r->t.n; i++) { /* (the first of equal ones) */
        const uint8_t *p = r->p8[i];
        int h = hash(p[0], p[1], p[2]);
        bool dup = false;
        for (; r->exact[h] >= 0 && !dup; h = (h + 1) & 511) {
            const uint8_t *q = r->p8[r->exact[h]];
            dup = q[0] == p[0] && q[1] == p[1] && q[2] == p[2];
        }
        if (!dup)
            r->exact[h] = (int16_t)i;
    }
    if (r->err)
        memset(r->err, 0, (uint32_t)(w + 2) * 3 * 2 * 2);
    if (palette)
        for (int i = 0; i < bi->colors; i++) {
            const uint8_t *c = bi->pal[i];
            bool same = i < t->keep_n && (c[0] >> 2) == t->pal[i][0] &&
                        (c[1] >> 2) == t->pal[i][1] &&
                        (c[2] >> 2) == t->pal[i][2];
            r->lut[i] = (uint8_t)(same ? i : nearest(r, c[0], c[1], c[2]));
        }
    r->out = out;
    r->out_w = out_w < w ? out_w : w;
    r->out_h = out_h;
    r->stride = stride;
    r->pos = bi->data_off;
    r->buf_at = 0xFFFFFFFF;
    return r;
}

const char *bmp_error(const bmp_reader_t *r) { return r ? r->error : NULL; }

/* RLE: one row of palette indexes (unset ones: 0) */
static void rle_row(bmp_reader_t *r) {
    int w = r->bi.w;
    bool four = r->bi.compression == 2;
    memset(r->row, 0, (uint32_t)w);
    if (r->rle_end)
        return;
    if (r->rle_blank > 0) { /* a delta jumped over this row */
        r->rle_blank--;
        return;
    }
    int x = r->rle_x;
    r->rle_x = 0;
    for (;;) {
        int n = next_byte(r), v = next_byte(r);
        if (n < 0 || v < 0) {
            r->rle_end = true;
            return;
        }
        if (n > 0) { /* a run: one colour (RLE4: two, in turn) */
            for (int i = 0; i < n; i++, x++)
                if (x < w)
                    r->row[x] = (uint8_t)(four ? (i & 1 ? v & 15 : v >> 4) : v);
            continue;
        }
        if (v == 0) /* the end of the row */
            return;
        if (v == 1) { /* the end of the picture */
            r->rle_end = true;
            return;
        }
        if (v == 2) { /* move right dx, down dy */
            int dx = next_byte(r), dy = next_byte(r);
            if (dx < 0 || dy < 0) {
                r->rle_end = true;
                return;
            }
            x += dx;
            if (dy > 0) { /* this row ends; dy - 1 blank ones; then at x */
                r->rle_blank = dy - 1;
                r->rle_x = x;
                return;
            }
            continue;
        }
        /* v pixels as they are, then to a 16-bit boundary */
        int bytes = four ? (v + 1) / 2 : v, b = 0;
        for (int i = 0; i < v; i++, x++) {
            if (!four || !(i & 1))
                b = next_byte(r);
            if (b < 0) {
                r->rle_end = true;
                return;
            }
            if (x < w)
                r->row[x] = (uint8_t)(four ? (i & 1 ? b & 15 : b >> 4) : b);
        }
        if (bytes & 1)
            next_byte(r);
    }
}

static uint8_t chan(const bmp_info_t *bi, uint32_t px, int k) {
    if (!bi->bits[k])
        return 0;
    uint32_t v = (px & bi->mask[k]) >> bi->shift[k];
    uint32_t top = (bi->bits[k] >= 32 ? 0xFFFFFFFFu : (1u << bi->bits[k]) - 1);
    return (uint8_t)(top ? v * 255u / top : 0);
}

/* a row as it is in the file: palette indexes, or R G B */
static void plain_row(bmp_reader_t *r) {
    const bmp_info_t *bi = &r->bi;
    int w = bi->w, b = bi->bpp;
    r->pos = bi->data_off + (uint32_t)r->done * bi->row_bytes;
    int acc = 0;
    for (int x = 0; x < w; x++) {
        int v;
        uint32_t px = 0;
        switch (b) {
        case 1:
            if (!(x & 7))
                acc = next_byte(r);
            r->row[x] = (uint8_t)(acc < 0 ? 0 : (acc >> (7 - (x & 7))) & 1);
            break;
        case 4:
            if (!(x & 1))
                acc = next_byte(r);
            r->row[x] = (uint8_t)(acc < 0 ? 0 : x & 1 ? acc & 15 : acc >> 4);
            break;
        case 8:
            v = next_byte(r);
            r->row[x] = (uint8_t)(v < 0 ? 0 : v);
            break;
        case 24: {
            int cb = next_byte(r), cg = next_byte(r), cr = next_byte(r);
            r->row[x * 3] = (uint8_t)(cr < 0 ? 0 : cr);
            r->row[x * 3 + 1] = (uint8_t)(cg < 0 ? 0 : cg);
            r->row[x * 3 + 2] = (uint8_t)(cb < 0 ? 0 : cb);
            break;
        }
        default: /* 16, 32: by the masks */
            for (int i = 0; i < b / 8; i++) {
                v = next_byte(r);
                px |= (uint32_t)(v < 0 ? 0 : v) << (8 * i);
            }
            for (int k = 0; k < 3; k++)
                r->row[x * 3 + k] = chan(bi, px, k);
        }
    }
}

static uint8_t clamp8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* the row to the target's colours, at picture row y */
static void map_row(bmp_reader_t *r, int y) {
    const bmp_info_t *bi = &r->bi;
    int w = bi->w;
    bool palette = bi->bpp <= 8, keep = y >= 0 && y < r->out_h;
    uint8_t *o = keep ? r->out + (uint32_t)y * (uint32_t)r->stride : NULL;
    if (!r->t.dither) {
        if (!keep)
            return;
        for (int x = 0; x < r->out_w; x++) {
            if (palette) {
                o[x] = r->lut[r->row[x] < bi->colors ? r->row[x] : 0];
            } else {
                const uint8_t *c = r->row + x * 3;
                o[x] = (uint8_t)lookup(r, c[0], c[1], c[2]);
            }
        }
        return;
    }
    /* Floyd-Steinberg: this row's errors in cur, the next row's in nxt */
    int16_t *cur = r->err + ((r->done & 1) ? (w + 2) * 3 : 0);
    int16_t *nxt = r->err + ((r->done & 1) ? 0 : (w + 2) * 3);
    memset(nxt, 0, (uint32_t)(w + 2) * 3 * 2);
    for (int x = 0; x < w; x++) {
        uint8_t src[3];
        if (palette) {
            int i = r->row[x] < bi->colors ? r->row[x] : 0;
            for (int k = 0; k < 3; k++)
                src[k] = bi->pal[i][k];
        } else {
            for (int k = 0; k < 3; k++)
                src[k] = r->row[x * 3 + k];
        }
        int16_t *e = cur + (x + 1) * 3;
        uint8_t v[3];
        for (int k = 0; k < 3; k++)
            v[k] = clamp8(src[k] + e[k] / 16);
        int idx;
        if (palette && !e[0] && !e[1] && !e[2])
            idx = r->lut[r->row[x] < bi->colors ? r->row[x] : 0];
        else
            idx = lookup(r, v[0], v[1], v[2]);
        if (o && x < r->out_w)
            o[x] = (uint8_t)idx;
        for (int k = 0; k < 3; k++) {
            int d = v[k] - r->p8[idx][k]; /* x16 below: errors in 1/16 */
            e[3 + k] += (int16_t)(d * 7);
            nxt[x * 3 + k] += (int16_t)(d * 3);
            nxt[(x + 1) * 3 + k] += (int16_t)(d * 5);
            nxt[(x + 2) * 3 + k] += (int16_t)d;
        }
    }
    memset(cur, 0, (uint32_t)(w + 2) * 3 * 2);
}

int bmp_rows(bmp_reader_t *r, int n) {
    if (!r || r->error)
        return r ? r->done : 0;
    const bmp_info_t *bi = &r->bi;
    for (int i = 0; i < n && r->done < bi->h; i++) {
        if (bi->compression == 1 || bi->compression == 2)
            rle_row(r);
        else
            plain_row(r);
        int y = bi->top_down ? r->done : bi->h - 1 - r->done;
        map_row(r, y);
        r->done++;
        bool rle = bi->compression == 1 || bi->compression == 2;
        if (r->short_read && !rle) /* (RLE: may end without its mark) */
            r->error = "The picture is cut short.";
    }
    return r->done;
}

// writing

/* the whole file in one go (made new) */
const char *bmp_save(const char *path, const uint8_t *pix, int w, int h,
                     int stride, const uint8_t (*pal)[3], int colors) {
    if (!path || !pix || !pal || w < 1 || h < 1 || w > BMP_MAX_SIDE ||
        h > BMP_MAX_SIDE)
        return "Nothing to save.";
    int bpp = colors <= 16 ? 4 : 8, n = bpp == 4 ? 16 : 256;
    uint32_t row = (uint32_t)(((w * bpp + 31) / 32) * 4);
    uint32_t off = 14 + 40 + (uint32_t)n * 4;
    uint8_t hd[14 + 40];
    memset(hd, 0, sizeof(hd));
    hd[0] = 'B', hd[1] = 'M';
    put32(hd + 2, off + row * (uint32_t)h);
    put32(hd + 10, off);
    put32(hd + 14, 40);
    put32(hd + 18, (uint32_t)w);
    put32(hd + 22, (uint32_t)h); /* bottom-up, as most programs expect */
    put16(hd + 26, 1);
    put16(hd + 28, (uint32_t)bpp);
    put32(hd + 34, row * (uint32_t)h);
    put32(hd + 38, 2835); /* 72 dots an inch */
    put32(hd + 42, 2835);
    put32(hd + 46, (uint32_t)n);
    static uint8_t p[256 * 4]; /* (a bit big for the stack) */
    for (int i = 0; i < n; i++) {
        p[i * 4] = bmp_8bit(pal[i][2]);
        p[i * 4 + 1] = bmp_8bit(pal[i][1]);
        p[i * 4 + 2] = bmp_8bit(pal[i][0]);
        p[i * 4 + 3] = 0;
    }
    /* rows, the bottom one first, about 8 KB at a time */
    int per = (int)(8192 / row) > 0 ? (int)(8192 / row) : 1;
    uint8_t *part = (uint8_t *)kmalloc((uint32_t)per * row);
    if (!part)
        return "Not enough memory to save.";
    dir_entry_t de;
    fat_file_t f;
    if ((fs_find(path, &de) && !fs_delete(path)) || !fs_create(path, &f)) {
        kfree(part);
        return "Save failed.";
    }
    bool ok = fs_write(&f, hd, sizeof(hd)) == (int)sizeof(hd) &&
              fs_write(&f, p, n * 4) == n * 4;
    for (int y = h - 1; y >= 0 && ok;) {
        int k = 0;
        memset(part, 0, (uint32_t)per * row);
        for (; k < per && y >= 0; k++, y--) {
            const uint8_t *s = pix + (uint32_t)y * (uint32_t)stride;
            uint8_t *d = part + (uint32_t)k * row;
            if (bpp == 8)
                memcpy(d, s, (uint32_t)w);
            else
                for (int x = 0; x < w; x++)
                    d[x / 2] |= (uint8_t)((s[x] & 15) << (x & 1 ? 0 : 4));
        }
        int len = (int)((uint32_t)k * row);
        ok = fs_write(&f, part, len) == len;
    }
    fs_close(&f);
    kfree(part);
    return ok ? NULL : "Save failed (the disk may be full).";
}
