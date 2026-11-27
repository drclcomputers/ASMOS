// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* The page: its HTML read once, start to end, and laid out as it goes:
 * words to the window's width, a line at a time, into items (a run of
 * text in one style, a box, a picture, a form field) at their places.
 * Laid out again when the window's width changes or a picture arrives
 * (what fields hold, and the pictures, are kept). HTML as it was around
 * 1996: headings, paragraphs, lists, <font>, <center>, tables as lines,
 * forms. Scripts and styles are skipped. */

#include "extapps/worldview/wv.h"

#define LEADING 2 /* between lines */
#define INDENT 16 /* lists, quotes */

enum { AL_LEFT, AL_CENTER, AL_RIGHT };

typedef struct {
    char tag[12]; /* what opened it ("" the page) */
    uint8_t font, scale, color, flags, align;
    bool pre;
    int16_t link;
    int16_t left, right; /* margins */
} style_t;

static struct {
    wv_doc_t *d;
    int width;
    int x;
    int32_t y;
    int line_first, line_h;
    bool line_has;
    uint8_t line_align; /* as when the line began (a </p> may end it) */
    int16_t line_left, line_right;
    int gap;    /* space owed before the next line */
    bool space; /* a space owed before the next word */
    int col;    /* <pre>: the column (tabs) */
    style_t st;
    style_t stack[40];
    int sp;
    int lists, list_n[10];
    bool list_ol[10];
    int bullet; /* owed at the next line's start: -1 a disc, n: "n." */
    int form;
    bool frames; /* a frameset seen: its frames are links */
    int nimg, nfld;
    /* what the last layout had: kept if the same */
    wv_image_t old_img[WV_IMAGES];
    int old_nimg;
    wv_field_t *old_fld;
    int old_nfld;
    const char *old_pool;
} L;

// the pool, the arrays

static bool grow(void **p, int *cap, int count, int size, int first) {
    if (count < *cap)
        return true;
    int ncap = *cap ? *cap * 2 : first;
    void *n = kmalloc((uint32_t)(ncap * size));
    if (!n)
        return false;
    if (*p)
        memcpy(n, *p, (uint32_t)(*cap * size));
    kfree(*p);
    *p = n;
    *cap = ncap;
    return true;
}

/* bytes into the pool: where they start (0xFFFFFFFF: no room) */
static uint32_t pool_add(const char *s, uint32_t n, bool nul) {
    wv_doc_t *d = L.d;
    uint32_t need = d->pool_len + n + (nul ? 1 : 0);
    if (need > d->pool_cap) {
        uint32_t cap = d->pool_cap ? d->pool_cap : 8192;
        while (cap < need)
            cap *= 2;
        char *np = (char *)kmalloc(cap);
        if (!np) {
            d->full = true;
            return 0xFFFFFFFF;
        }
        if (d->pool)
            memcpy(np, d->pool, d->pool_len);
        kfree(d->pool);
        d->pool = np;
        d->pool_cap = cap;
    }
    uint32_t at = d->pool_len;
    memcpy(d->pool + at, s, n);
    d->pool_len += n;
    if (nul)
        d->pool[d->pool_len++] = 0;
    return at;
}

static uint32_t pool_str(const char *s) {
    uint32_t r = pool_add(s, (uint32_t)strlen(s), true);
    return r == 0xFFFFFFFF ? 0 : r;
}

static wv_item_t *add_item(int kind, int w, int h) {
    wv_doc_t *d = L.d;
    if (d->full || !grow((void **)&d->items, &d->cap_items, d->nitems,
                         sizeof(wv_item_t), 256)) {
        d->full = true;
        return NULL;
    }
    wv_item_t *it = &d->items[d->nitems++];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->x = (int16_t)L.x;
    it->y = L.y;
    it->w = (int16_t)w;
    it->h = (int16_t)h;
    it->font = L.st.font;
    it->scale = L.st.scale;
    it->color = L.st.color;
    it->flags = L.st.flags;
    it->link = L.st.link;
    it->ref = -1;
    return it;
}

// lines

static int avail(void) { return L.st.right - L.st.left; }

static int line_gap_h(void) {
    return wv_font_h(L.st.font, L.st.scale) + LEADING;
}

/* the line done: its items at their height, centred if so */
static void finish_line(void) {
    wv_doc_t *d = L.d;
    if (!L.line_has)
        return;
    int lh = L.line_h;
    int right = L.line_left;
    for (int i = L.line_first; i < d->nitems; i++)
        if (d->items[i].x + d->items[i].w > right)
            right = d->items[i].x + d->items[i].w;
    int shift = 0;
    if (L.line_align == AL_CENTER)
        shift = (L.line_right - right) / 2;
    else if (L.line_align == AL_RIGHT)
        shift = L.line_right - right;
    shift = shift < 0 ? 0 : shift;
    for (int i = L.line_first; i < d->nitems; i++) {
        wv_item_t *it = &d->items[i];
        it->x = (int16_t)(it->x + shift);
        if (it->kind == IT_BOX && (it->flags & 0x80)) /* (a bullet) */
            it->y = L.y + (lh - LEADING - it->h) / 2;
        else
            it->y = L.y + lh - LEADING - it->h;
    }
    /* the line's items top to bottom (a tall picture after low text):
     * then the whole page's are, and doc_item_at_y can search them */
    for (int i = L.line_first + 1; i < d->nitems; i++) {
        wv_item_t t = d->items[i];
        int j = i - 1;
        while (j >= L.line_first && d->items[j].y > t.y) {
            d->items[j + 1] = d->items[j];
            j--;
        }
        d->items[j + 1] = t;
    }
    if (lh > d->max_h)
        d->max_h = lh;
    L.y += lh;
    L.x = L.st.left;
    L.line_first = d->nitems;
    L.line_h = 0;
    L.line_has = false;
    L.space = false;
    L.col = 0;
}

static void block(int gap) {
    finish_line();
    if (gap > L.gap)
        L.gap = gap;
    L.space = false;
}

/* <br>: the line ends; an empty one still takes a line */
static void line_break(void) {
    if (L.line_has)
        finish_line();
    else
        L.y += line_gap_h();
    L.x = L.st.left;
    L.space = false;
}

static void bullet_now(void) {
    if (!L.bullet)
        return;
    int b = L.bullet;
    L.bullet = 0;
    int fh = wv_font_h(L.st.font, L.st.scale);
    if (b < 0) {
        int sz = fh >= 8 ? 4 : 3;
        int x0 = L.x;
        L.x = L.st.left - 9;
        wv_item_t *it = add_item(IT_BOX, sz, sz);
        if (it) {
            it->flags = 0x80;
            it->color = L.st.color;
            it->link = -1;
        }
        L.x = x0;
    } else {
        char num[12];
        int n = snprintf(num, sizeof(num), "%d.", b);
        int w = wv_text_w(L.st.font, L.st.scale, num, n);
        uint32_t at = pool_add(num, (uint32_t)n, false);
        int x0 = L.x;
        L.x = L.st.left - w - 4;
        wv_item_t *it = at == 0xFFFFFFFF ? NULL : add_item(IT_TEXT, w, fh);
        if (it) {
            it->text = at;
            it->len = (uint16_t)n;
            it->link = -1;
            it->flags = 0;
        }
        L.x = x0;
    }
}

/* room for w more on this line (else a new one); the line's first thing
 * brings the space owed above it, and a list's bullet */
static void room(int w, int h) {
    if (L.line_has && L.x + w > L.st.right)
        finish_line();
    if (!L.line_has) {
        if (L.gap && L.y > 0)
            L.y += L.gap;
        L.gap = 0;
        L.x = L.st.left;
        L.line_first = L.d->nitems;
        L.line_has = true;
        L.line_align = L.st.align;
        L.line_left = L.st.left;
        L.line_right = L.st.right;
        bullet_now();
    }
    if (h + LEADING > L.line_h)
        L.line_h = h + LEADING;
}

// text

/* a word (code page bytes), with the space owed before it */
static void word(const char *w, int n) {
    int font = L.st.font, sc = L.st.scale;
    int fh = wv_font_h(font, sc);
    int ww = wv_text_w(font, sc, w, n);
    int sw = wv_char_w(font, sc, ' ');
    bool sp = L.space && L.line_has;
    if (L.line_has && L.x + (sp ? sw : 0) + ww > L.st.right) {
        finish_line();
        sp = false;
    }
    if (!L.line_has && ww > avail()) { /* longer than a line: in pieces */
        int k = 0, pw = 0;
        while (k < n && pw + wv_char_w(font, sc, (uint8_t)w[k]) <= avail())
            pw += wv_char_w(font, sc, (uint8_t)w[k++]);
        k = k ? k : 1;
        L.space = false;
        word(w, k);
        finish_line();
        if (k < n)
            word(w + k, n - k);
        return;
    }
    room(ww, fh);
    wv_doc_t *d = L.d;
    wv_item_t *last =
        d->nitems > L.line_first ? &d->items[d->nitems - 1] : NULL;
    bool join = last && last->kind == IT_TEXT && last->font == font &&
                last->scale == sc && last->color == L.st.color &&
                last->flags == L.st.flags && last->link == L.st.link &&
                last->x + last->w == L.x &&
                last->text + last->len == d->pool_len;
    if (sp && join) {
        if (pool_add(" ", 1, false) == 0xFFFFFFFF)
            return;
        last->len++;
        last->w = (int16_t)(last->w + sw);
        L.x += sw;
    } else if (sp) {
        L.x += sw;
    }
    uint32_t at = pool_add(w, (uint32_t)n, false);
    if (at == 0xFFFFFFFF)
        return;
    if (join) {
        last->len = (uint16_t)(last->len + n);
        last->w = (int16_t)(last->w + ww);
    } else {
        wv_item_t *it = add_item(IT_TEXT, ww, fh);
        if (!it)
            return;
        it->text = at;
        it->len = (uint16_t)n;
    }
    L.x += ww;
    L.space = false;
}

/* text as it is in the page (entities, UTF-8): words, spaces folded (in
 * <pre> kept, and its lines) */
static void text(const char *s, uint32_t n) {
    char wbuf[256];
    int wn = 0;
    uint32_t i = 0;
    while (i <= n) {
        uint32_t cp;
        if (i == n) {
            cp = 0;
            i++;
        } else if (s[i] == '&') {
            uint32_t j = i + 1;
            while (j < n && j - i < 12 && s[j] != ';' && s[j] != '&' &&
                   s[j] != '<' && s[j] != ' ')
                j++;
            cp = j < n && s[j] == ';' ? txt_entity(s + i + 1, (int)(j - i - 1))
                                      : 0;
            if (cp)
                i = j + 1;
            else {
                cp = '&';
                i++;
            }
        } else {
            cp = txt_next(s, n, &i);
        }
        bool white =
            cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0;
        if (L.st.pre && cp && (white || cp == 0xA0)) {
            if (wn)
                word(wbuf, wn);
            wn = 0;
            if (cp == '\n') {
                line_break();
            } else if (cp != '\r') {
                int k = cp == '\t' ? 8 - (L.col % 8) : 1;
                for (int t = 0; t < k; t++) {
                    L.space = false;
                    word(" ", 1);
                }
                L.col += k;
            }
            continue;
        }
        if (white) {
            if (wn)
                word(wbuf, wn);
            wn = 0;
            if (cp)
                L.space = true;
            continue;
        }
        char out[4];
        int k = txt_to_cp(cp, out);
        /* (&nbsp; comes as a ' ' here: in the word, holding it together) */
        for (int t = 0; t < k; t++) {
            if (wn >= (int)sizeof(wbuf) - 1) {
                word(wbuf, wn);
                wn = 0;
            }
            wbuf[wn++] = out[t];
            L.col++;
        }
    }
}

// attributes

/* attribute name's value in a tag (a: its text after the name); false if
 * it isn't there. raw: as written (entities still in) */
static bool attr_raw(const char *a, int n, const char *name, const char **v,
                     int *vn) {
    int nl = (int)strlen(name);
    int i = 0;
    while (i < n) {
        while (i < n && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n' ||
                         a[i] == '\r' || a[i] == '/'))
            i++;
        int s = i;
        while (i < n && a[i] != '=' && a[i] != ' ' && a[i] != '\t' &&
               a[i] != '\n' && a[i] != '\r' && a[i] != '>')
            i++;
        int namelen = i - s;
        while (i < n && (a[i] == ' ' || a[i] == '\t'))
            i++;
        const char *val = "";
        int vl = 0;
        if (i < n && a[i] == '=') {
            i++;
            while (i < n && (a[i] == ' ' || a[i] == '\t'))
                i++;
            if (i < n && (a[i] == '"' || a[i] == '\'')) {
                char q = a[i++];
                val = a + i;
                while (i < n && a[i] != q)
                    i++;
                vl = (int)(a + i - val);
                i++;
            } else {
                val = a + i;
                while (i < n && a[i] != ' ' && a[i] != '\t' && a[i] != '\n' &&
                       a[i] != '>')
                    i++;
                vl = (int)(a + i - val);
            }
        }
        if (namelen == nl && !strncasecmp(a + s, name, (size_t)nl)) {
            *v = val;
            *vn = vl;
            return true;
        }
        if (namelen == 0)
            i++;
    }
    return false;
}

/* as text: entities and UTF-8 to the code page */
static bool attr_text(const char *a, int n, const char *name, char *out,
                      int sz) {
    const char *v;
    int vn;
    out[0] = 0;
    if (!attr_raw(a, n, name, &v, &vn))
        return false;
    int o = 0;
    uint32_t i = 0;
    while (i < (uint32_t)vn && o < sz - 4) {
        uint32_t cp;
        if (v[i] == '&') {
            uint32_t j = i + 1;
            while (j < (uint32_t)vn && j - i < 12 && v[j] != ';')
                j++;
            cp = j < (uint32_t)vn ? txt_entity(v + i + 1, (int)(j - i - 1)) : 0;
            if (cp)
                i = j + 1;
            else {
                cp = '&';
                i++;
            }
        } else {
            cp = txt_next(v, (uint32_t)vn, &i);
        }
        o += txt_to_cp(cp, out + o);
    }
    out[o] = 0;
    return true;
}

/* as a URL: entities resolved, spaces and anything not ASCII %-encoded */
static bool attr_url(const char *a, int n, const char *name, char *out,
                     int sz) {
    const char *v;
    int vn;
    out[0] = 0;
    if (!attr_raw(a, n, name, &v, &vn))
        return false;
    static const char HEX[] = "0123456789ABCDEF";
    int o = 0;
    for (int i = 0; i < vn && o < sz - 4; i++) {
        uint8_t c = (uint8_t)v[i];
        if (c == '&') {
            int j = i + 1;
            while (j < vn && j - i < 12 && v[j] != ';')
                j++;
            uint32_t cp = j < vn ? txt_entity(v + i + 1, j - i - 1) : 0;
            if (cp && cp < 0x80) {
                c = (uint8_t)cp;
                i = j;
            }
        }
        if (c == '\n' || c == '\r' || c == '\t')
            continue;
        if (c == ' ' || c >= 0x80) {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 15];
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    /* (leading, trailing spaces gone) */
    while (o && out[o - 1] == ' ')
        out[--o] = 0;
    return true;
}

static int attr_int(const char *a, int n, const char *name, int def) {
    char b[16];
    if (!attr_text(a, n, name, b, sizeof(b)) || !b[0])
        return def;
    if (strchr(b, '%'))
        return def; /* (a share of the width: not known here) */
    return (int)str_to_uint32(b);
}

static const struct {
    const char *name;
    uint8_t r, g, b;
} COLORS[] = {{"black", 0, 0, 0},       {"white", 255, 255, 255},
              {"red", 255, 0, 0},       {"green", 0, 128, 0},
              {"blue", 0, 0, 255},      {"yellow", 255, 255, 0},
              {"navy", 0, 0, 128},      {"maroon", 128, 0, 0},
              {"purple", 128, 0, 128},  {"teal", 0, 128, 128},
              {"olive", 128, 128, 0},   {"gray", 128, 128, 128},
              {"grey", 128, 128, 128},  {"silver", 192, 192, 192},
              {"lime", 0, 255, 0},      {"aqua", 0, 255, 255},
              {"fuchsia", 255, 0, 255}, {"orange", 255, 165, 0},
              {"darkgreen", 0, 100, 0}, {"darkblue", 0, 0, 139},
              {"darkred", 139, 0, 0},   {"brown", 165, 42, 42},
              {"cyan", 0, 255, 255},    {"magenta", 255, 0, 255}};

static int hexv(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* a colour attribute: the screen's nearest, -1 if none or unknown */
static int attr_color(const char *a, int n, const char *name) {
    char b[24];
    if (!attr_text(a, n, name, b, sizeof(b)) || !b[0])
        return -1;
    const char *p = b[0] == '#' ? b + 1 : b;
    if (strlen(p) == 6) {
        int v[6], ok = 1;
        for (int i = 0; i < 6; i++)
            ok &= (v[i] = hexv(p[i])) >= 0;
        if (ok)
            return img_nearest(v[0] * 16 + v[1], v[2] * 16 + v[3],
                               v[4] * 16 + v[5]);
    }
    for (unsigned i = 0; i < sizeof(COLORS) / sizeof(COLORS[0]); i++)
        if (!strcasecmp(COLORS[i].name, b))
            return img_nearest(COLORS[i].r, COLORS[i].g, COLORS[i].b);
    return -1;
}

static int attr_align(const char *a, int n, int def) {
    char b[12], st[160];
    /* style="text-align: center" (the only style read) */
    if (attr_text(a, n, "style", st, sizeof(st))) {
        for (char *p = st; *p; p++)
            if (!strncasecmp(p, "text-align", 10)) {
                p += 10;
                while (*p == ' ' || *p == ':')
                    p++;
                int k = 0;
                while (k < (int)sizeof(b) - 1 &&
                       ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
                    b[k++] = *p++;
                b[k] = 0;
                if (!strcasecmp(b, "center"))
                    return AL_CENTER;
                if (!strcasecmp(b, "right"))
                    return AL_RIGHT;
                if (!strcasecmp(b, "left") || !strcasecmp(b, "justify"))
                    return AL_LEFT;
                break;
            }
    }
    if (!attr_text(a, n, "align", b, sizeof(b)))
        return def;
    if (!strcasecmp(b, "center") || !strcasecmp(b, "middle"))
        return AL_CENTER;
    if (!strcasecmp(b, "right"))
        return AL_RIGHT;
    if (!strcasecmp(b, "left") || !strcasecmp(b, "justify"))
        return AL_LEFT;
    return def;
}

// the style stack

static void push(const char *tag) {
    if (L.sp < (int)(sizeof(L.stack) / sizeof(L.stack[0])))
        L.stack[L.sp++] = L.st;
    strlcpy(L.st.tag, tag, sizeof(L.st.tag));
}

/* back to before the last tag (if it is open); true if it was */
static bool pop(const char *tag) {
    int k = L.sp - 1;
    if (strcmp(L.st.tag, tag)) {
        while (k >= 0 && strcmp(L.stack[k].tag, tag))
            k--;
        if (k < 0)
            return false;
        L.st = L.stack[k];
        L.sp = k;
        if (L.sp > 0) /* (the one under the tag) */
            L.st = L.stack[--L.sp];
        return true;
    }
    L.st = L.stack[--L.sp];
    return true;
}

static bool open_tag(const char *tag) {
    if (!strcmp(L.st.tag, tag))
        return true;
    for (int k = L.sp - 1; k >= 0; k--)
        if (!strcmp(L.stack[k].tag, tag))
            return true;
    return false;
}

// fonts

static void set_size(int level) { /* <font size>: 1..7 */
    level = level < 1 ? 1 : level > 7 ? 7 : level;
    if (level >= 6) {
        L.st.font = FONT_SYSTEM;
        L.st.scale = 2;
    } else if (level >= 4) {
        L.st.font = FONT_SYSTEM;
        L.st.scale = 1;
    } else if (L.st.font != FONT_TERM) {
        L.st.font = FONT_SMALL;
        L.st.scale = 1;
    }
}

static int size_level(void) {
    return L.st.scale > 1 ? 6 : L.st.font == FONT_SYSTEM ? 4 : 3;
}

// pictures, fields

static void image(const char *a, int n) {
    wv_doc_t *d = L.d;
    char src[WV_URL], url[WV_URL], alt[96];
    attr_url(a, n, "src", src, sizeof(src));
    attr_text(a, n, "alt", alt, sizeof(alt));
    if (!src[0] || d->nimages >= WV_IMAGES) {
        if (alt[0])
            text(alt, (uint32_t)strlen(alt));
        return;
    }
    url_join(d->base, src, url, sizeof(url));
    int k = d->nimages++;
    wv_image_t *im = &d->images[k];
    memset(im, 0, sizeof(*im));
    im->src = pool_str(url);
    im->alt = pool_str(alt);
    im->aw = (int16_t)attr_int(a, n, "width", 0);
    im->ah = (int16_t)attr_int(a, n, "height", 0);
    /* the same picture as last time: kept */
    if (k < L.old_nimg && L.old_pool &&
        !strcmp(L.old_pool + L.old_img[k].src, url)) {
        im->pix = L.old_img[k].pix;
        im->state = L.old_img[k].state;
        im->w = L.old_img[k].w;
        im->h = L.old_img[k].h;
        L.old_img[k].pix = NULL;
    }
    int w, h;
    if (im->pix) {
        w = im->w;
        h = im->h;
    } else if (im->aw > 0 && im->ah > 0) {
        w = im->aw;
        h = im->ah;
    } else {
        w = alt[0] ? wv_text_w(FONT_SMALL, 1, alt, (int)strlen(alt)) + 8 : 24;
        h = alt[0] ? 14 : 20;
        w = w > 160 ? 160 : w;
    }
    if (w > avail() && w > 0) { /* (too wide: smaller, the same shape) */
        h = h * avail() / w;
        w = avail();
    }
    w = w < 1 ? 1 : w;
    h = h < 1 ? 1 : h;
    room(w, h);
    wv_item_t *it = add_item(IT_IMAGE, w, h);
    if (it) {
        it->ref = (int16_t)k;
        it->flags = 0;
    }
    L.x += w;
    L.space = false;
}

static int field_new(int type, const char *a, int n) {
    wv_doc_t *d = L.d;
    if (!grow((void **)&d->fields, &d->cap_fields, d->nfields,
              sizeof(wv_field_t), 16))
        return -1;
    int k = d->nfields++;
    wv_field_t *f = &d->fields[k];
    memset(f, 0, sizeof(*f));
    f->type = (uint8_t)type;
    f->form = (int8_t)L.form;
    char b[WV_FIELD_TEXT];
    attr_text(a, n, "name", b, sizeof(b));
    f->name = pool_str(b);
    attr_text(a, n, "value", f->value, sizeof(f->value));
    f->initial = pool_str(f->value);
    f->checked = attr_raw(a, n, "checked", &(const char *){0}, &(int){0});
    f->size = (int16_t)attr_int(a, n, "size", 20);
    f->size = f->size < 2 ? 2 : f->size > 60 ? 60 : f->size;
    f->sel = 0;
    /* what it held before this layout: kept */
    int j = L.nfld++;
    if (j < L.old_nfld && L.old_fld && L.old_pool &&
        L.old_fld[j].type == type &&
        !strcmp(L.old_pool + L.old_fld[j].name, doc_str(d, f->name))) {
        memcpy(f->value, L.old_fld[j].value, sizeof(f->value));
        f->checked = L.old_fld[j].checked;
        f->sel = L.old_fld[j].sel;
    }
    return k;
}

static void field_item(int k, int w, int h) {
    if (k < 0)
        return;
    w = w > avail() ? avail() : w;
    if (L.space && L.line_has)
        L.x += wv_char_w(L.st.font, L.st.scale, ' ');
    room(w, h);
    wv_item_t *it = add_item(IT_FIELD, w, h);
    if (it) {
        it->ref = (int16_t)k;
        it->link = -1;
        it->flags = 0;
        it->font = FONT_SMALL;
        it->scale = 1;
    }
    L.x += w;
    L.space = true;
}

static int button_w(const char *label) {
    return wv_text_w(FONT_SMALL, 1, label, (int)strlen(label)) + 12;
}

static void input(const char *a, int n) {
    char type[16];
    if (!attr_text(a, n, "type", type, sizeof(type)) || !type[0])
        strcpy(type, "text");
    int t = !strcasecmp(type, "hidden")     ? FLD_HIDDEN
            : !strcasecmp(type, "password") ? FLD_PASSWORD
            : !strcasecmp(type, "submit") || !strcasecmp(type, "image") ||
                    !strcasecmp(type, "button")
                ? FLD_SUBMIT
            : !strcasecmp(type, "reset")    ? FLD_RESET
            : !strcasecmp(type, "checkbox") ? FLD_CHECK
            : !strcasecmp(type, "radio")    ? FLD_RADIO
                                            : FLD_TEXT;
    int k = field_new(t, a, n);
    if (k < 0 || t == FLD_HIDDEN)
        return;
    wv_field_t *f = &L.d->fields[k];
    if (t == FLD_SUBMIT || t == FLD_RESET) {
        if (!f->value[0]) {
            char alt[64];
            attr_text(a, n, "alt", alt, sizeof(alt));
            strlcpy(f->value,
                    alt[0]           ? alt
                    : t == FLD_RESET ? "Reset"
                                     : "Submit",
                    sizeof(f->value));
        }
        field_item(k, button_w(f->value), 13);
    } else if (t == FLD_CHECK || t == FLD_RADIO) {
        if (!f->value[0])
            strcpy(f->value, "on");
        field_item(k, 9, 9);
    } else {
        field_item(k, f->size * wv_char_w(FONT_SMALL, 1, 'n') + 6, 13);
    }
}

/* where the tag `name` closes, from i (or the end) */
static uint32_t find_close(const char *h, uint32_t len, uint32_t i,
                           const char *name) {
    int nl = (int)strlen(name);
    for (; i + 2 + (uint32_t)nl <= len; i++)
        if (h[i] == '<' && h[i + 1] == '/' &&
            !strncasecmp(h + i + 2, name, (size_t)nl))
            return i;
    return len;
}

/* past the tag that starts at i */
static uint32_t tag_end(const char *h, uint32_t len, uint32_t i) {
    char q = 0;
    for (; i < len; i++) {
        if (q) {
            if (h[i] == q)
                q = 0;
        } else if (h[i] == '"' || h[i] == '\'') {
            q = h[i];
        } else if (h[i] == '>') {
            return i + 1;
        }
    }
    return len;
}

/* text between i and j without its tags, as the code page */
static void plain(const char *h, uint32_t i, uint32_t j, char *out, int sz,
                  bool keep_lines) {
    int o = 0;
    bool sp = false;
    while (i < j && o < sz - 4) {
        if (h[i] == '<') {
            i = tag_end(h, j, i);
            continue;
        }
        uint32_t cp;
        if (h[i] == '&') {
            uint32_t k = i + 1;
            while (k < j && k - i < 12 && h[k] != ';')
                k++;
            cp = k < j ? txt_entity(h + i + 1, (int)(k - i - 1)) : 0;
            if (cp)
                i = k + 1;
            else {
                cp = '&';
                i++;
            }
        } else {
            cp = txt_next(h, j, &i);
        }
        if (cp == '\n' && keep_lines) {
            out[o++] = '\n';
            sp = false;
            continue;
        }
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
            sp = o > 0;
            continue;
        }
        if (sp)
            out[o++] = ' ';
        sp = false;
        o += txt_to_cp(cp, out + o);
    }
    out[o] = 0;
}

/* <select>: its options, to </select>; returns where to go on */
static uint32_t select_field(const char *h, uint32_t len, uint32_t i,
                             const char *a, int n) {
    uint32_t end = find_close(h, len, i, "select");
    int k = field_new(FLD_SELECT, a, n);
    if (k < 0)
        return tag_end(h, len, end);
    wv_field_t *f = &L.d->fields[k];
    int keep_sel = f->sel;
    f->opts = L.d->pool_len;
    f->nopt = 0;
    int widest = 20, chosen = -1;
    uint32_t p = i;
    while (p < end && f->nopt < 64) {
        if (h[p] == '<' && !strncasecmp(h + p + 1, "option", 6)) {
            uint32_t te = tag_end(h, end, p);
            const char *oa = h + p + 7;
            int on = (int)(te - (p + 7));
            uint32_t next = p + 1;
            while (next < end && !(h[next] == '<' &&
                                   (!strncasecmp(h + next + 1, "option", 6) ||
                                    !strncasecmp(h + next + 1, "/option", 7) ||
                                    !strncasecmp(h + next + 1, "/select", 7))))
                next++;
            char label[80], value[WV_FIELD_TEXT];
            plain(h, te, next, label, sizeof(label), false);
            if (!attr_text(oa, on, "value", value, sizeof(value)))
                strlcpy(value, label, sizeof(value));
            if (attr_raw(oa, on, "selected", &(const char *){0}, &(int){0}))
                chosen = f->nopt;
            pool_add(value, (uint32_t)strlen(value), true);
            pool_add(label, (uint32_t)strlen(label), true);
            int w = wv_text_w(FONT_SMALL, 1, label, (int)strlen(label));
            widest = w > widest ? w : widest;
            f->nopt++;
            p = next;
            continue;
        }
        p++;
    }
    if (k < L.old_nfld && L.old_fld && L.old_fld[k].type == FLD_SELECT)
        f->sel = (int16_t)keep_sel;
    else
        f->sel = (int16_t)(chosen >= 0 ? chosen : 0);
    field_item(k, widest + 16, 13);
    return tag_end(h, len, end);
}

static uint32_t textarea_field(const char *h, uint32_t len, uint32_t i,
                               const char *a, int n) {
    uint32_t end = find_close(h, len, i, "textarea");
    int k = field_new(FLD_TEXTAREA, a, n);
    if (k >= 0) {
        wv_field_t *f = &L.d->fields[k];
        if (k >= L.old_nfld) {
            plain(h, i, end, f->value, sizeof(f->value), false);
            f->initial = pool_str(f->value);
        }
        int cols = attr_int(a, n, "cols", 30), rows = attr_int(a, n, "rows", 3);
        cols = cols < 4 ? 4 : cols > 80 ? 80 : cols;
        rows = rows < 1 ? 1 : rows > 8 ? 8 : rows;
        f->rows = (int16_t)rows;
        f->size = (int16_t)cols;
        field_item(k, cols * wv_char_w(FONT_SMALL, 1, 'n') + 6, rows * 9 + 4);
    }
    return tag_end(h, len, end);
}

static uint32_t button_field(const char *h, uint32_t len, uint32_t i,
                             const char *a, int n) {
    uint32_t end = find_close(h, len, i, "button");
    char type[12];
    attr_text(a, n, "type", type, sizeof(type));
    int t = !strcasecmp(type, "reset") ? FLD_RESET : FLD_SUBMIT;
    int k = field_new(t, a, n);
    if (k >= 0) {
        wv_field_t *f = &L.d->fields[k];
        char label[64];
        plain(h, i, end, label, sizeof(label), false);
        if (!f->value[0])
            strlcpy(f->value, label[0] ? label : "Submit", sizeof(f->value));
        field_item(k, button_w(label[0] ? label : f->value), 13);
        f->initial = pool_str(label[0] ? label : f->value); /* (its label) */
    }
    return tag_end(h, len, end);
}

// tags

static bool is(const char *t, const char *name) { return !strcmp(t, name); }

static void frame_link(wv_doc_t *d, const char *a, int n, bool inline_);

/* the heading levels: font, scale, the space around them */
static void heading(int level, const char *a, int n) {
    block(level <= 2 ? 10 : 8);
    push(level == 1 ? "h1" : level == 2 ? "h2" : "h3");
    L.st.font = FONT_SYSTEM;
    L.st.scale = level <= 2 ? 2 : 1;
    L.st.align = (uint8_t)attr_align(a, n, L.st.align);
}

/* a tag: what it does; returns where to go on (past what it took) */
static uint32_t tag(const char *t, bool close, const char *a, int n,
                    const char *h, uint32_t len, uint32_t next) {
    wv_doc_t *d = L.d;
    /* blocks */
    if (is(t, "p")) {
        if (open_tag("p"))
            pop("p");
        block(close ? 6 : 8);
        if (!close) {
            push("p");
            L.st.align = (uint8_t)attr_align(a, n, L.st.align);
        }
    } else if (is(t, "div") || is(t, "center") || is(t, "address") ||
               is(t, "article") || is(t, "section") || is(t, "header") ||
               is(t, "footer") || is(t, "nav") || is(t, "main") ||
               is(t, "aside") || is(t, "figure") || is(t, "figcaption") ||
               is(t, "form") || is(t, "table") || is(t, "fieldset")) {
        bool form = is(t, "form");
        block(form || is(t, "table") ? 4 : 0);
        if (close) {
            pop(t);
            if (form)
                L.form = -1;
        } else {
            push(t);
            if (is(t, "center"))
                L.st.align = AL_CENTER;
            else
                L.st.align = (uint8_t)attr_align(a, n, L.st.align);
            if (form && d->nforms < WV_FORMS) {
                char act[WV_URL], url[WV_URL], m[8];
                attr_url(a, n, "action", act, sizeof(act));
                url_join(d->base, act, url, sizeof(url));
                attr_text(a, n, "method", m, sizeof(m));
                d->forms[d->nforms].action = pool_str(url);
                d->forms[d->nforms].post = !strcasecmp(m, "post");
                L.form = d->nforms++;
            }
        }
    } else if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) {
        if (close) {
            if (!pop("h1") && !pop("h2"))
                pop("h3");
            block(6);
        } else {
            heading(t[1] - '0', a, n);
        }
    } else if (is(t, "br")) {
        line_break();
    } else if (is(t, "hr")) {
        block(4);
        int w = avail();
        int aw = attr_int(a, n, "width", 0);
        w = aw > 0 && aw < w ? aw : w;
        room(w, 2);
        wv_item_t *it = add_item(IT_BOX, w, 2);
        if (it) {
            it->color = DARK_GRAY;
            it->link = -1;
            it->flags = 0;
        }
        L.x += w;
        finish_line();
        block(4);
    } else if (is(t, "ul") || is(t, "ol") || is(t, "menu") || is(t, "dir")) {
        block(L.lists ? 0 : 6);
        if (close) {
            if (pop(is(t, "ol") ? "ol" : "ul") && L.lists)
                L.lists--;
        } else {
            push(is(t, "ol") ? "ol" : "ul");
            L.st.left = (int16_t)(L.st.left + INDENT + 4);
            if (L.lists < 10) {
                L.list_ol[L.lists] = is(t, "ol");
                L.list_n[L.lists] = attr_int(a, n, "start", 1) - 1;
            }
            L.lists++;
        }
    } else if (is(t, "li")) {
        block(0);
        if (!close && L.lists) {
            int k = L.lists - 1 < 10 ? L.lists - 1 : 9;
            L.bullet = L.list_ol[k] ? ++L.list_n[k] : -1;
        }
    } else if (is(t, "dl")) {
        block(6);
        if (close)
            pop("dd");
    } else if (is(t, "dt") || is(t, "dd")) {
        if (open_tag("dd"))
            pop("dd");
        block(0);
        if (!close && is(t, "dd")) {
            push("dd");
            L.st.left = (int16_t)(L.st.left + INDENT);
        }
    } else if (is(t, "blockquote")) {
        block(6);
        if (close)
            pop(t);
        else {
            push(t);
            L.st.left = (int16_t)(L.st.left + INDENT);
            L.st.right = (int16_t)(L.st.right - INDENT > L.st.left + 40
                                       ? L.st.right - INDENT
                                       : L.st.right);
        }
    } else if (is(t, "pre") || is(t, "listing") || is(t, "xmp") ||
               is(t, "plaintext")) {
        block(6);
        if (close)
            pop("pre");
        else {
            push("pre");
            L.st.pre = true;
            L.st.font = FONT_TERM;
            L.st.scale = 1;
        }
    } else if (is(t, "tr")) {
        block(0);
    } else if (is(t, "td") || is(t, "th")) {
        if (!close && L.line_has) {
            L.x += 8;
            L.space = false;
        }
        if (is(t, "th")) {
            if (close)
                pop("th");
            else {
                push("th");
                L.st.font = FONT_SYSTEM;
            }
        }
    } else if (is(t, "caption")) {
        block(2);
    }
    /* inline */
    else if (is(t, "b") || is(t, "strong")) {
        if (close)
            pop("b");
        else {
            push("b");
            if (L.st.font == FONT_SMALL)
                L.st.font = FONT_SYSTEM;
        }
    } else if (is(t, "u") || is(t, "ins")) {
        if (close)
            pop("u");
        else {
            push("u");
            L.st.flags |= IF_UNDER;
        }
    } else if (is(t, "s") || is(t, "strike") || is(t, "del")) {
        if (close)
            pop("s");
        else {
            push("s");
            L.st.flags |= IF_STRIKE;
        }
    } else if (is(t, "tt") || is(t, "code") || is(t, "kbd") || is(t, "samp")) {
        if (close)
            pop("tt");
        else {
            push("tt");
            L.st.font = FONT_TERM;
            L.st.scale = 1;
        }
    } else if (is(t, "font")) {
        if (close)
            pop("font");
        else {
            push("font");
            char sz[8];
            if (attr_text(a, n, "size", sz, sizeof(sz)) && sz[0]) {
                int lv = sz[0] == '+'   ? 3 + (int)str_to_uint32(sz + 1)
                         : sz[0] == '-' ? 3 - (int)str_to_uint32(sz + 1)
                                        : (int)str_to_uint32(sz);
                set_size(lv);
            }
            int c = attr_color(a, n, "color");
            if (c >= 0 && L.st.link < 0)
                L.st.color = (uint8_t)c;
        }
    } else if (is(t, "big")) {
        if (close)
            pop("big");
        else {
            push("big");
            set_size(size_level() + 1);
        }
    } else if (is(t, "small")) {
        if (close)
            pop("small");
        else {
            push("small");
            set_size(size_level() - 1);
        }
    } else if (is(t, "a")) {
        if (close) {
            pop("a");
        } else {
            if (open_tag("a")) /* (an <a> not closed: this ends it) */
                pop("a");
            push("a");
            char href[WV_URL], url[WV_URL];
            if (attr_url(a, n, "href", href, sizeof(href)) && href[0] &&
                strncasecmp(href, "javascript:", 11) &&
                grow((void **)&d->links, &d->cap_links, d->nlinks,
                     sizeof(uint32_t), 64)) {
                url_join(d->base, href, url, sizeof(url));
                d->links[d->nlinks] = pool_str(url);
                L.st.link = (int16_t)d->nlinks++;
                bool seen = d->visited && d->visited(d->user, url);
                L.st.color = seen ? d->vlink_c : d->link_c;
                L.st.flags |= IF_UNDER;
            }
        }
    } else if (is(t, "img") && !close) {
        image(a, n);
    } else if (is(t, "input") && !close) {
        input(a, n);
    } else if (is(t, "select") && !close) {
        return select_field(h, len, next, a, n);
    } else if (is(t, "textarea") && !close) {
        return textarea_field(h, len, next, a, n);
    } else if (is(t, "button") && !close) {
        return button_field(h, len, next, a, n);
    } else if (is(t, "body") && !close) {
        int c;
        if ((c = attr_color(a, n, "bgcolor")) >= 0)
            d->bg = (uint8_t)c;
        if ((c = attr_color(a, n, "text")) >= 0)
            L.st.color = d->fg = (uint8_t)c;
        if ((c = attr_color(a, n, "link")) >= 0)
            d->link_c = (uint8_t)c;
        if ((c = attr_color(a, n, "vlink")) >= 0)
            d->vlink_c = (uint8_t)c;
        if (d->fg == d->bg) /* (unreadable: the usual) */
            L.st.color = d->fg = d->bg == BLACK ? WHITE : BLACK;
    } else if (is(t, "base") && !close) {
        char href[WV_URL];
        if (attr_url(a, n, "href", href, sizeof(href)) && href[0]) {
            char url[WV_URL];
            url_join(d->base, href, url, sizeof(url));
            strlcpy(d->base, url, sizeof(d->base));
        }
    } else if (is(t, "title") && !close) {
        uint32_t end = find_close(h, len, next, "title");
        if (!d->title[0])
            plain(h, next, end, d->title, sizeof(d->title), false);
        return tag_end(h, len, end);
    } else if (is(t, "frame") && !close) {
        frame_link(d, a, n, false);
    } else if (is(t, "iframe") && !close) { /* a link, what's in it skipped */
        frame_link(d, a, n, true);
        return tag_end(h, len, find_close(h, len, next, t));
    } else if ((is(t, "script") || is(t, "style") || is(t, "svg") ||
                is(t, "template") || is(t, "object") || is(t, "noembed") ||
                is(t, "noframes")) &&
               !close) {
        /* (noframes: for browsers without frames; frames are links here,
         * so the "your browser doesn't do frames" text isn't shown) */
        return tag_end(h, len, find_close(h, len, next, t));
    }
    return next;
}

/* a frame (or an iframe): a link to its page, as Lynx shows them. Its
 * name: title=, else name=, else the file it shows */
static void frame_link(wv_doc_t *d, const char *a, int n, bool inline_) {
    char src[WV_URL], url[WV_URL], label[80];
    if (!attr_url(a, n, "src", src, sizeof(src)) || !src[0] ||
        !strncasecmp(src, "javascript:", 11) || !strcmp(src, "about:blank"))
        return;
    url_join(d->base, src, url, sizeof(url));
    if (!attr_text(a, n, "title", label, sizeof(label)) || !label[0]) {
        if (!attr_text(a, n, "name", label, sizeof(label)) || !label[0]) {
            const char *b = strrchr(src, '/');
            strlcpy(label, b && b[1] ? b + 1 : src, sizeof(label));
        }
    }
    if (!inline_) {
        if (!L.frames) { /* the first: what this page is */
            L.frames = true;
            block(6);
            text("This page is in frames. Each one opens as a page:", 51);
            block(4);
        }
        block(0);
        text("Frame: ", 7);
    } else {
        text(" [Frame: ", 9);
    }
    if (grow((void **)&d->links, &d->cap_links, d->nlinks, sizeof(uint32_t),
             64)) {
        push("a");
        d->links[d->nlinks] = pool_str(url);
        L.st.link = (int16_t)d->nlinks++;
        bool seen = d->visited && d->visited(d->user, url);
        L.st.color = seen ? d->vlink_c : d->link_c;
        L.st.flags |= IF_UNDER;
        text(label, (uint32_t)strlen(label));
        pop("a");
    }
    if (inline_)
        text("] ", 2);
}

// the page

static void reset_doc(wv_doc_t *d) {
    d->nitems = 0;
    d->pool_len = 0;
    d->nlinks = 0;
    d->nimages = 0;
    d->nfields = 0;
    d->nforms = 0;
    d->max_h = 0;
    d->full = false;
    d->title[0] = 0;
}

void doc_layout(wv_doc_t *d, int width) {
    /* what it had: to keep pictures and what fields hold */
    memset(&L, 0, sizeof(L));
    L.d = d;
    L.old_pool = d->pool;
    L.old_nimg = d->nimages;
    memcpy(L.old_img, d->images, sizeof(wv_image_t) * (uint32_t)d->nimages);
    L.old_fld = d->fields;
    L.old_nfld = d->nfields;
    d->pool = NULL;
    d->pool_cap = 0;
    d->fields = NULL;
    d->cap_fields = 0;
    reset_doc(d);
    if (!d->bg && !d->fg) {
        d->bg = WHITE;
        d->fg = BLACK;
    }
    if (!d->link_c)
        d->link_c = BLUE;
    if (!d->vlink_c)
        d->vlink_c = MAGENTA;

    width = width < 60 ? 60 : width;
    d->width = width;
    L.width = width;
    L.st.font = FONT_SMALL;
    L.st.scale = 1;
    L.st.color = d->fg;
    L.st.link = -1;
    L.st.left = 4;
    L.st.right = (int16_t)(width - 4);
    L.x = 4;
    L.y = 4;
    L.form = -1;
    pool_add("", 0, true); /* (offset 0: "") */

    const char *h = d->html;
    uint32_t len = d->html_len, i = 0;
    while (h && i < len && !d->full) {
        if (h[i] != '<') {
            uint32_t j = i;
            while (j < len && h[j] != '<')
                j++;
            text(h + i, j - i);
            i = j;
            continue;
        }
        if (!strncmp(h + i, "<!--", 4)) {
            const char *e =
                (const char *)memmem(h + i + 4, len - i - 4, "-->", 3);
            i = e ? (uint32_t)(e - h) + 3 : len;
            continue;
        }
        if (i + 1 < len && (h[i + 1] == '!' || h[i + 1] == '?')) {
            i = tag_end(h, len, i);
            continue;
        }
        bool close = i + 1 < len && h[i + 1] == '/';
        uint32_t p = i + 1 + (close ? 1 : 0);
        char t[12];
        int tn = 0;
        while (p < len && tn < (int)sizeof(t) - 1 &&
               ((h[p] >= 'a' && h[p] <= 'z') || (h[p] >= 'A' && h[p] <= 'Z') ||
                (h[p] >= '0' && h[p] <= '9')))
            t[tn++] = (char)(h[p++] | 0x20);
        t[tn] = 0;
        if (!tn) { /* a "<" on its own: text */
            text("<", 1);
            i++;
            continue;
        }
        while (p < len && ((h[p] >= 'a' && h[p] <= 'z') ||
                           (h[p] >= 'A' && h[p] <= 'Z') || h[p] == '-'))
            p++; /* (a longer name than kept) */
        uint32_t end = tag_end(h, len, p);
        int an = (int)(end - p) - (end <= len && end > p && h[end - 1] == '>');
        i = tag(t, close, h + p, an < 0 ? 0 : an, h, len, end);
    }
    finish_line();
    d->height = L.y + 8;

    /* what was not kept */
    for (int k = 0; k < L.old_nimg; k++)
        kfree(L.old_img[k].pix);
    kfree((void *)L.old_pool);
    kfree(L.old_fld);
}

void doc_free(wv_doc_t *d) {
    for (int k = 0; k < d->nimages; k++)
        kfree(d->images[k].pix);
    kfree(d->items);
    kfree(d->pool);
    kfree(d->links);
    kfree(d->fields);
    kfree(d->html);
    memset(d, 0, sizeof(*d));
}

int doc_item_at_y(const wv_doc_t *d, int32_t y) {
    int lo = 0, hi = d->nitems;
    while (lo < hi) { /* (the first that could reach down to y: none is
                       * taller than max_h) */
        int mid = (lo + hi) / 2;
        if (d->items[mid].y + d->max_h < y)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int doc_form_query(const wv_doc_t *d, int form, int submit, char *out, int sz) {
    int o = 0;
    out[0] = 0;
    for (int k = 0; k < d->nfields; k++) {
        const wv_field_t *f = &d->fields[k];
        const char *name = doc_str(d, f->name);
        if (f->form != form || !name[0])
            continue;
        const char *v = f->value;
        if ((f->type == FLD_SUBMIT && k != submit) || f->type == FLD_RESET)
            continue;
        if ((f->type == FLD_CHECK || f->type == FLD_RADIO) && !f->checked)
            continue;
        if (f->type == FLD_SELECT) {
            const char *p = doc_str(d, f->opts);
            for (int i = 0; i < f->sel && i < f->nopt; i++) {
                p += strlen(p) + 1;
                p += strlen(p) + 1;
            }
            v = f->nopt ? p : "";
        }
        if (o && o < sz - 1)
            out[o++] = '&';
        o += url_encode(name, out + o, sz - o);
        if (o < sz - 1)
            out[o++] = '=';
        o += url_encode(v, out + o, sz - o);
    }
    out[o < sz ? o : sz - 1] = 0;
    return o;
}
