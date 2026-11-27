// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef LIB_IMAGE_H
#define LIB_IMAGE_H

/* Pictures from memory: GIF (its first frame, transparency, interlaced
 * or not), JPEG (baseline, grey or colour, any of the usual samplings,
 * restart markers; not progressive) and BMP (1 to 32 bits, not
 * compressed: lib/bmp.h reads the rest, from files). For the kernel and
 * (through the app API's `image` table, API size 15) the apps: the same
 * names either way.
 *
 * The picture comes out at the size asked for (smaller: each pixel the
 * nearest of the picture's), as indexes of a palette (the screen's): the
 * nearest colour, or with an ordered dither. A JPEG made an eighth of its
 * size or less is read from its blocks' averages (fast). The memory it
 * works in (some KB to tens of KB) is taken while it decodes, and given
 * back; an app's comes from its own heap.
 *
 * The structures below are shared with the apps: their layout stays. */

#include "lib/core.h"

#define IMAGE_GIF 'G'
#define IMAGE_JPEG 'J'
#define IMAGE_BMP 'B'
#define IMAGE_PNG 'P' /* known, not decoded */

typedef struct {
    const uint8_t (*pal)[3]; /* the colours to use, 0..63 each */
    int n;                   /* the nearest is looked for in pal[0..n-1] */
    bool dither;             /* ordered dither (16 colours: needed) */
    uint8_t bg[3];           /* what transparent shows (R, G, B 0..255) */
    /* the memory it works in (NULL: the kernel's heap) */
    void *(*alloc)(uint32_t n);
    void (*free)(void *p);
} image_target_t;

/* what it is, from its first bytes: IMAGE_*, 0 if none of them */
int image_kind(const uint8_t *b, uint32_t n);
/* its size: NULL, else why not ("Not a picture it knows.") */
const char *image_info(const uint8_t *b, uint32_t n, int *w, int *h);
/* decoded to dw x dh into out (rows of stride bytes): NULL, else why not
 * ("A progressive JPEG: not shown.") */
const char *image_decode(const uint8_t *b, uint32_t n, uint8_t *out, int dw,
                         int dh, int stride, const image_target_t *t);

#endif
