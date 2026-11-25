// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef LIB_BMP_H
#define LIB_BMP_H

/* Windows .BMP pictures, for the kernel and (through the app API's `bmp`
 * table, API size 11) the apps: the same names either way.
 *
 * Reads 1, 4 and 8 bits a pixel (with a palette; also RLE4 and RLE8),
 * 16, 24 and 32 (colours in the pixels; 16 and 32 also with their own bit
 * masks), bottom-up or top-down, Windows or OS/2 headers. The pixels come
 * out as indexes of the screen's colours: the nearest one of each, or with
 * Floyd-Steinberg dithering. Reading goes a few rows at a time
 * (bmp_rows), so a big picture can fill in over several frames.
 *
 * Writes 4 bits a pixel (16 colours) or 8 (256), with a given palette.
 *
 * The structures below are shared with the apps: their layout stays. */

#include "lib/core.h"

#define BMP_MAX_SIDE 8192

typedef struct {
    int w, h;
    int bpp;              /* 1, 4, 8, 16, 24 or 32 */
    int colors;           /* entries in its palette (0: none) */
    uint32_t compression; /* 0 none, 1 RLE8, 2 RLE4, 3 bit masks */
    bool top_down;        /* the first row in the file is the top one */
    uint8_t pal[256][3];  /* its palette: R, G, B, 8 bits each */
    /* (for the reader) */
    uint32_t data_off, row_bytes, mask[3];
    int shift[3], bits[3];
    uint32_t reserved[4];
} bmp_info_t;

/* the header: NULL if it can be read, else why not ("Not a BMP picture.") */
const char *bmp_open(const char *path, bmp_info_t *bi);

/* what the pixels become */
typedef struct {
    const uint8_t (*pal)[3]; /* the screen's colours, 0..63 each */
    int n;                   /* the nearest is looked for in pal[0..n-1] */
    /* a palette index i < keep_n whose colour is pal[i] (pal has keep_n
     * entries) stays i: a picture saved by ASMOS comes back the same */
    int keep_n;
    bool dither;
} bmp_target_t;

typedef struct bmp_reader bmp_reader_t;

/* The memory a reader needs for this picture and target (some 40 KB for
 * 24 bits, less for a palette). */
uint32_t bmp_work_size(const bmp_info_t *bi, const bmp_target_t *t);
/* Starts reading into out, in `work` (bmp_work_size bytes, the caller's,
 * kept until the reading is over): the picture's row y at out + y *
 * stride, the first out_w pixels of it, rows from 0 to out_h - 1 (the rest
 * is read and left out). */
bmp_reader_t *bmp_begin(const char *path, const bmp_info_t *bi,
                        const bmp_target_t *t, uint8_t *out, int out_w,
                        int out_h, int stride, void *work);
/* Reads up to n more rows; returns how many rows are done so far (of
 * bi->h). Done when it reaches bi->h, or bmp_error says why it stopped. */
int bmp_rows(bmp_reader_t *r, int n);
const char *bmp_error(const bmp_reader_t *r); /* NULL: no trouble */

/* Saves w x h pixels (rows of stride bytes, top first) with the palette
 * pal (0..63 each): colors 16 makes a 4-bit file (only indexes 0..15 may
 * be used), 256 an 8-bit one. NULL if saved, else why not. */
const char *bmp_save(const char *path, const uint8_t *pix, int w, int h,
                     int stride, const uint8_t (*pal)[3], int colors);

/* a 6-bit colour (0..63) as 8 bits, the way bmp_save writes it */
static inline uint8_t bmp_8bit(uint8_t v) {
    return (uint8_t)((v << 2) | (v >> 4));
}

/* The usual way, with the work memory from kmalloc (the kernel's heap, or
 * an app's own): bmp_start ... bmp_rows ... bmp_end, or all at once. */
void *kmalloc(uint32_t size);
void kfree(void *p);

static inline bmp_reader_t *bmp_start(const char *path, const bmp_info_t *bi,
                                      const bmp_target_t *t, uint8_t *out,
                                      int out_w, int out_h, int stride) {
    void *work = kmalloc(bmp_work_size(bi, t));
    bmp_reader_t *r =
        work ? bmp_begin(path, bi, t, out, out_w, out_h, stride, work) : NULL;
    if (!r && work)
        kfree(work);
    return r;
}

static inline void bmp_end(bmp_reader_t *r) {
    if (r)
        kfree(r); /* (the reader is the start of its work memory) */
}

/* All of it at once (small pictures). NULL if fine, else why not. */
static inline const char *bmp_read_all(const char *path, const bmp_info_t *bi,
                                       const bmp_target_t *t, uint8_t *out,
                                       int out_w, int out_h, int stride) {
    bmp_reader_t *r = bmp_start(path, bi, t, out, out_w, out_h, stride);
    if (!r)
        return "Not enough memory to read the picture.";
    while (bmp_rows(r, 64) < bi->h && !bmp_error(r))
        ;
    const char *e = bmp_error(r);
    bmp_end(r);
    return e;
}

#endif
