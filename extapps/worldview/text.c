// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* Characters: the web's (UTF-8; older pages Windows-1252) to the bytes
 * the fonts draw (the code page chosen in Settings), with plain stand-ins
 * for what it doesn't have (curly quotes -> straight, accents dropped).
 * And the fonts: widths kept here (a page is measured word by word), and
 * glyphs drawn twice as big for headings. */

#include "extapps/worldview/wv.h"

// characters

/* Windows-1252's 0x80..0x9F (0: unused) */
static const uint16_t CP1252[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

uint32_t txt_next(const char *s, uint32_t len, uint32_t *i) {
    const uint8_t *p = (const uint8_t *)s + *i;
    uint32_t left = len - *i;
    uint8_t c = p[0];
    if (c < 0x80) {
        (*i)++;
        return c;
    }
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (n && (uint32_t)n <= left) {
        uint32_t cp = c & (0x7F >> n);
        bool ok = true;
        for (int k = 1; k < n && ok; k++) {
            ok = (p[k] & 0xC0) == 0x80;
            cp = cp << 6 | (p[k] & 0x3F);
        }
        if (ok) {
            *i += (uint32_t)n;
            return cp;
        }
    }
    (*i)++; /* not UTF-8: Windows-1252 */
    if (c < 0xA0)
        return CP1252[c - 0x80] ? CP1252[c - 0x80] : '?';
    return c;
}

/* letters without their accents: U+00C0..U+00FF, U+0100..U+017F */
static const char LATIN1[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
static const char LATIN_A[] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNn"
    "nNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

static int cp_byte(uint32_t cp) {
    const uint16_t *t = app_codepage();
    for (int i = 0; t && i < 128; i++)
        if (t[i] == cp)
            return 0x80 + i;
    return -1;
}

static int put(char out[4], const char *s) {
    int n = 0;
    while (s[n] && n < 4) {
        out[n] = s[n];
        n++;
    }
    return n;
}

int txt_to_cp(uint32_t cp, char out[4]) {
    if (cp == '\t' || cp == '\n' || cp == '\r')
        cp = ' ';
    if (cp < 0x20)
        return 0;
    if (cp < 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    int b = cp_byte(cp);
    if (b < 0 && cp >= 0x218 && cp <= 0x21B) /* s, t with a comma below: the
                                              * ones with a cedilla */
        b = cp_byte(cp == 0x218   ? 0x15E
                    : cp == 0x219 ? 0x15F
                    : cp == 0x21A ? 0x162
                                  : 0x163);
    if (b >= 0 && b != 0xA0) {
        out[0] = (char)b;
        return 1;
    }
    switch (cp) {
    case 0xA0:
    case 0x2002:
    case 0x2003:
    case 0x2009:
        return put(out, " ");
    case 0xAD:
    case 0x200B:
    case 0x200C:
    case 0x200D:
    case 0xFEFF:
        return 0; /* (nothing to see) */
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x2032:
    case 0xB4:
        return put(out, "'");
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x2033:
    case 0xAB:
    case 0xBB:
        return put(out, "\"");
    case 0x2010:
    case 0x2011:
    case 0x2013:
    case 0x2014:
    case 0x2212:
        return put(out, "-");
    case 0x2026:
        return put(out, "...");
    case 0x2022:
    case 0xB7:
    case 0x2027:
        return put(out, "*");
    case 0x2122:
        return put(out, "TM");
    case 0xA9:
        return put(out, "(c)");
    case 0xAE:
        return put(out, "(R)");
    case 0x20AC:
        return put(out, "EUR");
    case 0xA3:
        return put(out, "GBP");
    case 0x2190:
        return put(out, "<-");
    case 0x2192:
        return put(out, "->");
    case 0x2039:
        return put(out, "<");
    case 0x203A:
        return put(out, ">");
    case 0xB0:
        return put(out, "o");
    }
    if (cp >= 0xC0 && cp <= 0xFF) {
        out[0] = LATIN1[cp - 0xC0];
        return 1;
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        out[0] = LATIN_A[cp - 0x100];
        return 1;
    }
    if (cp >= 0x218 && cp <= 0x21B) {
        out[0] = "SsTt"[cp - 0x218];
        return 1;
    }
    out[0] = '?';
    return 1;
}

int txt_cp_to_utf8(uint8_t b, char out[4]) {
    uint32_t cp = b;
    if (b >= 0x80) {
        const uint16_t *t = app_codepage();
        cp = t && t[b - 0x80] ? t[b - 0x80] : '?';
    }
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | cp >> 12);
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

// entities

/* U+00A0..U+00FF, in order */
static const char *const LATIN1_NAMES =
    "nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not "
    "shy reg macr deg plusmn sup2 sup3 acute micro para middot cedil sup1 "
    "ordm raquo frac14 frac12 frac34 iquest Agrave Aacute Acirc Atilde Auml "
    "Aring AElig Ccedil Egrave Eacute Ecirc Euml Igrave Iacute Icirc Iuml "
    "ETH Ntilde Ograve Oacute Ocirc Otilde Ouml times Oslash Ugrave Uacute "
    "Ucirc Uuml Yacute THORN szlig agrave aacute acirc atilde auml aring "
    "aelig ccedil egrave eacute ecirc euml igrave iacute icirc iuml eth "
    "ntilde ograve oacute ocirc otilde ouml divide oslash ugrave uacute "
    "ucirc uuml yacute thorn yuml ";

static const struct {
    const char *name;
    uint16_t cp;
} OTHER[] = {
    {"amp", '&'},       {"lt", '<'},        {"gt", '>'},
    {"quot", '"'},      {"apos", '\''},     {"ndash", 0x2013},
    {"mdash", 0x2014},  {"lsquo", 0x2018},  {"rsquo", 0x2019},
    {"sbquo", 0x201A},  {"ldquo", 0x201C},  {"rdquo", 0x201D},
    {"bdquo", 0x201E},  {"bull", 0x2022},   {"hellip", 0x2026},
    {"trade", 0x2122},  {"euro", 0x20AC},   {"dagger", 0x2020},
    {"Dagger", 0x2021}, {"permil", 0x2030}, {"lsaquo", 0x2039},
    {"rsaquo", 0x203A}, {"OElig", 0x152},   {"oelig", 0x153},
    {"Scaron", 0x160},  {"scaron", 0x161},  {"Yuml", 0x178},
    {"fnof", 0x192},    {"circ", 0x2C6},    {"tilde", 0x2DC},
    {"ensp", 0x2002},   {"emsp", 0x2003},   {"thinsp", 0x2009},
    {"zwnj", 0x200C},   {"zwj", 0x200D},    {"larr", 0x2190},
    {"rarr", 0x2192},   {"uarr", 0x2191},   {"darr", 0x2193},
    {"minus", 0x2212},  {"prime", 0x2032},  {"Prime", 0x2033},
    {"middot", 0xB7},   {"nbsp", 0xA0},     {"shy", 0xAD},
};

uint32_t txt_entity(const char *name, int n) {
    if (n < 2 || n > 10)
        return 0;
    if (name[0] == '#') {
        uint32_t v = 0;
        bool hex = name[1] == 'x' || name[1] == 'X';
        for (int i = hex ? 2 : 1; i < n; i++) {
            char c = name[i];
            int d = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
            if (d < 0 || (!hex && d > 9))
                return 0;
            v = v * (hex ? 16 : 10) + (uint32_t)d;
            if (v > 0x10FFFF)
                return 0;
        }
        if (v >= 0x80 && v < 0xA0) /* (pages mean Windows-1252 there) */
            v = CP1252[v - 0x80] ? CP1252[v - 0x80] : '?';
        return v;
    }
    for (unsigned i = 0; i < sizeof(OTHER) / sizeof(OTHER[0]); i++)
        if ((int)strlen(OTHER[i].name) == n &&
            !strncmp(OTHER[i].name, name, (size_t)n))
            return OTHER[i].cp;
    const char *p = LATIN1_NAMES;
    for (uint32_t cp = 0xA0; *p; cp++) {
        const char *e = strchr(p, ' ');
        if (e - p == n && !strncmp(p, name, (size_t)n))
            return cp;
        p = e + 1;
    }
    return 0;
}

// fonts: widths (and glyphs, for big text) kept

#define NF 4
static const int FONTS[NF] = {FONT_SMALL, FONT_SYSTEM, FONT_TERM, FONT_MONO8};
static int8_t s_w[NF][256];       /* ink widths, -1 not asked yet */
static uint8_t s_rows[2][256][8]; /* FONT_SMALL, FONT_SYSTEM glyphs */
static bool s_have_rows[2][256];
static bool s_ready;

static int font_slot(int font) {
    for (int i = 0; i < NF; i++)
        if (FONTS[i] == font)
            return i;
    return 0;
}

static void ready(void) {
    if (s_ready)
        return;
    memset(s_w, -1, sizeof(s_w));
    s_ready = true;
}

static int ink(int font, uint8_t c) {
    ready();
    int k = font_slot(font);
    if (s_w[k][c] < 0) {
        uint8_t rows[8];
        int w = 0;
        font_glyph(font, c, rows, &w);
        s_w[k][c] = (int8_t)w;
        if (k < 2) {
            memcpy(s_rows[k][c], rows, 8);
            s_have_rows[k][c] = true;
        }
    }
    return s_w[k][c];
}

int wv_char_w(int font, int scale, uint8_t c) {
    int adv = font == FONT_TERM    ? FONT_TERM_W
              : font == FONT_MONO8 ? 8
              : font == FONT_MONO  ? 5
                                   : ink(font, c) + 1;
    return adv * scale;
}

int wv_text_w(int font, int scale, const char *s, int n) {
    int w = 0;
    for (int i = 0; i < n; i++)
        w += wv_char_w(font, scale, (uint8_t)s[i]);
    return w;
}

int wv_font_h(int font, int scale) {
    static int h[NF];
    int k = font_slot(font);
    if (!h[k])
        h[k] = font_height(font);
    return h[k] * scale;
}

void wv_draw_text(int x, int y, const char *s, int n, uint8_t color, int font,
                  int scale) {
    if (scale <= 1) {
        draw_text_n(x, y, s, n, color, font);
        return;
    }
    int k = font_slot(font) < 2 ? font_slot(font) : 1;
    int rows = font_height(k ? FONT_SYSTEM : FONT_SMALL);
    for (int i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        int adv = wv_char_w(font, scale, c);
        if (!s_have_rows[k][c])
            ink(k ? FONT_SYSTEM : FONT_SMALL, c);
        for (int r = 0; r < rows && r < 8; r++) {
            uint8_t bits = s_rows[k][c][r];
            for (int col = 0; bits; col++, bits <<= 1) {
                if (!(bits & 0x80))
                    continue;
                int run = 1; /* (a run of dots: one rectangle) */
                while ((uint8_t)(bits << run) & 0x80)
                    run++;
                fill_rect(x + col * scale, y + r * scale, run * scale, scale,
                          color);
                bits = (uint8_t)(bits << (run - 1));
                col += run - 1;
            }
        }
        x += adv;
    }
}
