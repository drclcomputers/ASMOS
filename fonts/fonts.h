#ifndef FONTS_H
#define FONTS_H

#include "lib/types.h"

/* The `size` argument of draw_string()/draw_char()/text_width() picks the
 * font: */
#define FONT_MONO8 1  /* 8x8 fixed (terminal, big text)            */
#define FONT_MONO 2   /* 4x6 fixed: the classic UI font, editors   */
#define FONT_SYSTEM 3 /* bold, proportional, 8 rows: titles, menus */
#define FONT_SMALL 4  /* proportional, 7 rows: labels, status bars */

extern unsigned char font1_data[];
extern unsigned char font2_data[];

/* proportional font: glyphs 32..126, one byte per row, bit 7 = left */
typedef struct {
    uint8_t h;            /* rows per glyph */
    const uint8_t *width; /* ink width; advance is width + 1 */
    const uint8_t *bits;  /* 95 * h bytes */
} pfont_t;

extern const pfont_t font_system; /* fonts/font_prop.c (tools/mkfont.py) */
extern const pfont_t font_small;

#endif
