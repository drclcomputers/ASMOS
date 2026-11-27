// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* World View: a web browser for the web that old machines can still read
 * (FrogFind, 68k.news, plain HTTP sites, or anything through a proxy that
 * does the HTTPS). HTML 3.2-ish: text, links, headings, lists, forms,
 * GIF, BMP and JPEG pictures. No scripts, no style sheets.
 *
 *   http.c   URLs, and HTTP without waiting (one step a frame)
 *   text.c   characters (UTF-8, Windows-1252, entities -> the code page),
 *            fonts: measuring, and drawing twice as big
 *   html.c   the page: parsed and laid out to the window's width
 *   image.c  GIF, BMP, JPEG -> the screen's colours, made to fit
 *   main.c   the window: toolbar, address, page, history, settings */

#ifndef WV_H
#define WV_H

#ifdef WV_SIM /* (the simulator's tests look inside: the kernel's headers) */
#include "fonts/fonts.h"
#include "lib/core.h"
#include "ui/controls.h"
#include "ui/window.h"
#else
#include "extapps/shim.h"
#endif

#define WV_URL 400                     /* a URL, at most */
#define WV_HTML_MAX (160u * 1024u)     /* a page, at most */
#define WV_IMG_FILE_MAX (192u * 1024u) /* a picture's file, at most */
#define WV_IMG_PIX_MAX (96u * 1024u)   /* a picture on the page, at most */
#define WV_IMAGES 48                   /* pictures on a page */
#define WV_FORMS 16
#define WV_HISTORY 24     /* pages Back goes through */
#define WV_FIELD_TEXT 128 /* what a field holds */

// URLs (http.c)

typedef struct {
    bool https;
    char host[80];
    uint16_t port;
    char path[WV_URL]; /* from the first '/', with the query */
} wv_url_t;

bool url_split(const char *url, wv_url_t *u);
/* rel (a link, a form's action) against base: an absolute URL */
void url_join(const char *base, const char *rel, char *out, int sz);
/* application/x-www-form-urlencoded: bytes in the code page go out as
 * UTF-8; returns the length (out is cut at sz) */
int url_encode(const char *in, char *out, int sz);

// fetching (http.c): fetch_step every frame until DONE or FAILED

enum { F_IDLE, F_DNS, F_CONNECT, F_SEND, F_RECV, F_DONE, F_FAILED };

typedef struct {
    int state;
    char url[WV_URL]; /* the one asked for (redirected: the last) */
    int dns, sock;
    uint8_t ip[4];
    uint16_t port;  /* where it connects (the site's, or the proxy's) */
    uint32_t since; /* ms: when the step began, or data last came */
    char *req;
    int req_len, req_sent;
    char *post; /* a form's body (POST), or NULL */
    int post_len;
    /* the answer */
    char head[2048];
    int head_len;
    bool head_done;
    int status;
    char ctype[48];   /* "text/html" (lower case, no parameters) */
    char charset[24]; /* from Content-Type, or "" */
    char location[WV_URL];
    bool chunked;
    int32_t clen; /* -1: until the end */
    int chunk_left, chunk_st;
    char chunk_line[20];
    int chunk_line_len;
    uint8_t *body;
    uint32_t len, cap, max;
    bool truncated; /* more than max: cut */
    int redirects;
    char err[96];
} wv_fetch_t;

typedef struct {
    char proxy[80]; /* "host:port", "" none */
} wv_net_cfg_t;
extern wv_net_cfg_t g_wv_net;

void fetch_start(wv_fetch_t *f, const char *url, const char *post, int post_len,
                 uint32_t max);
int fetch_step(wv_fetch_t *f);
void fetch_free(wv_fetch_t *f); /* (stops it, if it runs) */
/* what it is doing, for the status line */
void fetch_status(const wv_fetch_t *f, char *out, int sz);

// characters and fonts (text.c)

/* the next character of text (UTF-8, or else Windows-1252): its code
 * point; *i moves past it */
uint32_t txt_next(const char *s, uint32_t len, uint32_t *i);
/* a code point as the code page's bytes (a byte, or a few ASCII ones for
 * what it doesn't have: quotes, dashes, accents dropped...): how many */
int txt_to_cp(uint32_t cp, char out[4]);
/* an entity's code point ("amp", "#38", "#x26"), 0 if unknown */
uint32_t txt_entity(const char *name, int n);
/* a byte of the code page as UTF-8 (forms): how many bytes */
int txt_cp_to_utf8(uint8_t b, char out[4]);

int wv_char_w(int font, int scale, uint8_t c);
int wv_text_w(int font, int scale, const char *s, int n);
int wv_font_h(int font, int scale);
void wv_draw_text(int x, int y, const char *s, int n, uint8_t color, int font,
                  int scale);

// the page (html.c)

enum { IT_TEXT, IT_BOX, IT_IMAGE, IT_FIELD };
#define IF_UNDER 1  /* underlined (links) */
#define IF_STRIKE 2 /* struck through */

typedef struct {
    int32_t y;
    int16_t x, w, h;
    uint8_t kind, font, scale, color;
    uint8_t flags;
    int16_t link; /* -1 none */
    int16_t ref;  /* the picture, or the field */
    uint32_t text;
    uint16_t len;
} wv_item_t;

enum { IMG_WAIT, IMG_LOADING, IMG_DONE, IMG_FAILED };

typedef struct {
    uint32_t src;   /* its URL (in the pool) */
    int16_t aw, ah; /* width= height= (0: not given) */
    int16_t w, h;   /* as shown */
    uint8_t *pix;   /* w x h in the screen's colours, NULL until loaded */
    uint8_t state;
    uint32_t alt; /* alt= (in the pool) */
} wv_image_t;

enum {
    FLD_TEXT,
    FLD_PASSWORD,
    FLD_HIDDEN,
    FLD_SUBMIT,
    FLD_RESET,
    FLD_CHECK,
    FLD_RADIO,
    FLD_SELECT,
    FLD_TEXTAREA
};

typedef struct {
    uint8_t type;
    int8_t form;      /* -1: outside a form */
    uint32_t name;    /* (pool) */
    uint32_t initial; /* value= (pool): what Reset puts back */
    char value[WV_FIELD_TEXT];
    bool checked;
    int16_t size; /* characters across */
    int16_t rows; /* a textarea's */
    /* a select's options: "value\0label\0" each, in the pool */
    uint32_t opts;
    int16_t nopt, sel;
} wv_field_t;

typedef struct {
    uint32_t action; /* (pool) */
    bool post;
} wv_form_t;

typedef struct {
    char *html; /* the source, kept to lay it out again */
    uint32_t html_len;
    char base[WV_URL];
    char title[96];
    uint8_t bg, fg, link_c, vlink_c;
    int width; /* laid out for */
    int32_t height;
    wv_item_t *items;
    int nitems, cap_items;
    char *pool; /* the text shown, and the URLs and names */
    uint32_t pool_len, pool_cap;
    uint32_t *links; /* each link's URL (pool) */
    int nlinks, cap_links;
    wv_image_t images[WV_IMAGES];
    int nimages;
    wv_field_t *fields;
    int nfields, cap_fields;
    wv_form_t forms[WV_FORMS];
    int nforms;
    bool full;     /* out of memory: the page is cut */
    int16_t max_h; /* the tallest line (doc_item_at_y) */
    /* the app's: is a URL visited (link colour) */
    bool (*visited)(void *user, const char *url);
    void *user;
} wv_doc_t;

/* lay the page out (again) at width; pictures and what fields hold are
 * kept if the source is the same */
void doc_layout(wv_doc_t *d, int width);
void doc_free(wv_doc_t *d);
/* a string in the page's pool ("" if off is out of it) */
static inline const char *doc_str(const wv_doc_t *d, uint32_t off) {
    return d->pool && off < d->pool_len ? d->pool + off : "";
}
/* the first item at or below y (they come in order) */
int doc_item_at_y(const wv_doc_t *d, int32_t y);
/* a form's fields as a query string, the button submit pressed */
int doc_form_query(const wv_doc_t *d, int form, int submit, char *out, int sz);

// pictures (image.c)

/* what kind (from the bytes): 'G' GIF, 'B' BMP, 'J' JPEG, 0 */
int img_kind(const uint8_t *b, uint32_t n);
/* its size (0 if it can't tell) */
bool img_size(const uint8_t *b, uint32_t n, int *w, int *h);
/* decoded to dw x dh in the screen's colours (bg: what transparent
 * shows); NULL if it can't (*why says) */
uint8_t *img_decode(const uint8_t *b, uint32_t n, int dw, int dh, uint8_t bg,
                    const char **why);
/* the screen's colours: once, and when they change */
void img_palette_init(void);
/* the screen's nearest colour to r, g, b (0..255) */
uint8_t img_nearest(int r, int g, int b);

// the app (main.c)

#define HOME_URL "http://frogfind.com/"
#define VISITED_N 256
#define TOOL_H 17 /* the toolbar's height */

typedef struct {
    char url[WV_URL];
    int32_t scroll;
} hist_t;

typedef struct {
    window *win; /* must stay first */
    control *btn[5], *addr, *view, *vs, *status;
    wv_doc_t doc;
    wv_fetch_t page, pic;
    int pic_k;    /* the picture being fetched, -1 none */
    bool loading; /* the page */
    char url[WV_URL];
    char title[128];
    hist_t hist[WV_HISTORY];
    int nhist, at;
    bool from_history; /* (Back, Forward: no new entry) */
    int32_t scroll, want_scroll;
    int hover; /* the link under the mouse, -1 */
    int focus; /* the field being typed in, -1 */
    char home[WV_URL];
    bool images;
    uint32_t visited[VISITED_N];
    int nvisited;
    bool relayout;
    uint32_t relayout_at;
    char line[160]; /* the status line */
    int laid_w;
    int menu_field; /* a select's options shown */
    uint32_t blink;
} wv_state_t;

#endif
