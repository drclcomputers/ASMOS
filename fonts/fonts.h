// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef FONTS_H
#define FONTS_H

#include "lib/types.h"

/* The `size` argument of draw_string()/draw_char()/text_width() picks the
 * font: */
#define FONT_MONO8 1  /* 8x8 fixed (terminal, big text)            */
#define FONT_MONO 2   /* 4x6 fixed: the classic UI font, editors   */
#define FONT_SYSTEM 3 /* bold, proportional, 8 rows: titles, menus */
#define FONT_SMALL 4  /* proportional, 7 rows: labels, status bars */
#define FONT_TERM 5   /* the Small glyphs in fixed 6 px cells: terminals */
#define FONT_TERM_W 6

extern unsigned char font1_data[];
extern unsigned char font2_data[];

typedef struct {
    uint8_t h;            /* rows per glyph */
    const uint8_t *width; /* ink width; advance is width + 1 */
    const uint8_t *bits;  /* 95 * h bytes */
} pfont_t;

extern const pfont_t font_system;
extern const pfont_t font_small;

#define CP_WESTERN 0 /* Windows-1252: Western Europe */
#define CP_CENTRAL 1 /* Windows-1250: Central Europe (Romanian...) */
#define CP_DOS 2     /* code page 437: DOS, box drawing */
#define CP_COUNT 3
extern const pfont_t ext_mono8, ext_mono, ext_system, ext_small;
extern const uint8_t cp_glyph[CP_COUNT][128];    /* 0xFF: nothing there */
extern const uint16_t cp_unicode[CP_COUNT][128]; /* 0: nothing there */
extern const char *const cp_name[CP_COUNT];
extern int g_codepage; /* CP_*, from the settings (lib/graphics.c) */

/* byte c has a glyph (printable ASCII, or one of the code page) */
bool font_has_char(unsigned char c);

#endif
