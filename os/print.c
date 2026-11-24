// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "os/print.h"
#include "config/runtime_config.h"
#include "drivers/lpt.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

const char *const PRN_TYPE_NAMES[PRN_TYPES] = {
    "Text only", "Epson ESC/P (dot matrix)", "HP PCL (LaserJet)"};

static print_job_t s_jobs[PRINT_MAX_JOBS];
static int s_njobs, s_next_id = 1;
static bool s_paused;
static uint32_t s_rev;
static lpt_status_t s_last = LPT_READY; /* the printer's last answer */

// output buffer

typedef struct {
    uint8_t *p;
    uint32_t len, cap;
    bool fail;
} buf_t;

static void put(buf_t *b, const void *d, uint32_t n) {
    if (b->fail)
        return;
    if (b->len + n > b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n)
            cap *= 2;
        uint8_t *np = krealloc(b->p, cap);
        if (!np) {
            b->fail = true;
            return;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void puts_(buf_t *b, const char *s) { put(b, s, (uint32_t)strlen(s)); }
static void putc_(buf_t *b, uint8_t c) { put(b, &c, 1); }

// text layout

typedef struct {
    const char *t;
    uint32_t len, i;
    bool ff; /* a form feed ended the last line */
} wrap_t;

/* next printed line; false at the end of the text */
static bool next_line(wrap_t *w, char out[PRINT_COLS + 1]) {
    w->ff = false;
    if (w->i >= w->len)
        return false;
    int col = 0, last_space = -1;
    uint32_t space_i = 0;
    while (w->i < w->len) {
        char c = w->t[w->i];
        if (c == '\n') {
            w->i++;
            break;
        }
        if (c == '\f') {
            w->i++;
            w->ff = true;
            break;
        }
        if (c == '\r') {
            w->i++;
            continue;
        }
        if (c == '\t') {
            int to = (col / 8 + 1) * 8;
            if (to > PRINT_COLS)
                to = PRINT_COLS;
            while (col < to)
                out[col++] = ' ';
            w->i++;
            if (col >= PRINT_COLS)
                break;
            continue;
        }
        if (col >= PRINT_COLS) { /* full: break at a space if one */
            if (last_space > PRINT_COLS / 2) {
                col = last_space;
                w->i = space_i + 1;
            }
            break;
        }
        if (c == ' ') {
            last_space = col;
            space_i = w->i;
        }
        out[col++] =
            ((unsigned char)c < 32 || (unsigned char)c > 126) ? '?' : c;
        w->i++;
    }
    out[col] = 0;
    return true;
}

/* Calls `fn` for each line, with its page; stops early if fn says so. */
static int layout(const char *t, uint32_t len,
                  bool (*fn)(void *u, int page, int line, const char *s),
                  void *u) {
    wrap_t w = {t, len, 0, false};
    char line[PRINT_COLS + 1];
    int page = 1, ln = 0;
    if (!len)
        return 1;
    while (next_line(&w, line)) {
        if (ln == PRINT_LINES) {
            page++;
            ln = 0;
        }
        if (fn && !fn(u, page, ln, line))
            return page;
        ln++;
        if (w.ff && w.i < len) {
            page++;
            ln = 0;
        }
    }
    return page;
}

int print_text_pages(const char *text, uint32_t len) {
    return layout(text, len, NULL, NULL);
}

typedef struct {
    int page, n;
    char (*lines)[PRINT_COLS + 1];
} page_get_t;

static bool get_page(void *u, int page, int line, const char *s) {
    page_get_t *g = u;
    if (page < g->page)
        return true;
    if (page > g->page)
        return false;
    strcpy(g->lines[line], s);
    g->n = line + 1;
    return true;
}

int print_text_page(const char *text, uint32_t len, int page,
                    char lines[][PRINT_COLS + 1]) {
    page_get_t g = {page, 0, lines};
    layout(text, len, get_page, &g);
    return g.n;
}

typedef struct {
    buf_t *b;
    int from, to, cur;
    bool started;
} text_out_t;

static bool emit_line(void *u, int page, int line, const char *s) {
    text_out_t *o = u;
    (void)line;
    if (page < o->from)
        return true;
    if (o->to && page > o->to)
        return false;
    if (o->started && page != o->cur)
        putc_(o->b, '\f');
    o->started = true;
    o->cur = page;
    puts_(o->b, s);
    puts_(o->b, "\r\n");
    return true;
}

// pictures

/* the standard 16 colours, as luminance */
static const uint8_t GREY[16] = {0,  20,  100, 120, 51,  71,  91,  170,
                                 85, 110, 192, 215, 140, 162, 240, 255};

uint8_t print_grey(uint8_t c) { return GREY[c & 15]; }

bool print_ink(uint8_t grey, int x, int y) {
    static const uint8_t BAYER[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    return grey < BAYER[y & 3][x & 3] * 16 + 8;
}

/* ESC/P: 80 dots an inch across (ESC * 4, the "CRT" density: 640 dots
 * on 8"), 72 down; PCL at 100 dpi (800 dots on 8"). Text: characters. */
void print_image_scale(int w, int h, int type, int *sx, int *sy) {
    int across = type == PRN_PCL ? 800 : 640;
    int s = w > 0 ? across / w : 1;
    if (s < 1)
        s = 1;
    if (s > 4)
        s = 4;
    (void)h;
    *sx = *sy = s;
}

static bool dot(const uint8_t *pix, int w, int h, int stride, int s, int x,
                int y) {
    int px = x / s, py = y / s;
    if (px >= w || py >= h)
        return false;
    return print_ink(print_grey(pix[py * stride + px]), x, y);
}

static void image_escp(buf_t *b, const uint8_t *pix, int w, int h, int stride) {
    int s, sy;
    print_image_scale(w, h, PRN_ESCP, &s, &sy);
    int dw = w * s, dh = h * s;
    put(b, "\x1b@", 2); /* reset */
    put(b,
        "\x1b"
        "3\x18",
        3); /* line feed 24/216" = 8 dots */
    for (int band = 0; band < dh; band += 8) {
        uint8_t hdr[5] = {0x1B, '*', 4, (uint8_t)(dw & 0xFF),
                          (uint8_t)(dw >> 8)};
        put(b, hdr, 5);
        for (int x = 0; x < dw; x++) {
            uint8_t col = 0;
            for (int k = 0; k < 8; k++) /* top pin = bit 7 */
                if (dot(pix, w, h, stride, s, x, band + k))
                    col |= (uint8_t)(0x80 >> k);
            putc_(b, col);
        }
        put(b, "\r\n", 2);
    }
    put(b, "\x1b@", 2);
}

static void image_pcl(buf_t *b, const uint8_t *pix, int w, int h, int stride) {
    int s, sy;
    print_image_scale(w, h, PRN_PCL, &s, &sy);
    int dw = w * s, dh = h * s, nb = (dw + 7) / 8;
    char t[24];
    puts_(b, "\x1b"
             "E");          /* reset */
    puts_(b, "\x1b*t100R"); /* 100 dpi */
    puts_(b, "\x1b*r1A");   /* start raster at the cursor */
    uint8_t row[128];
    for (int y = 0; y < dh; y++) {
        memset(row, 0, sizeof(row));
        for (int x = 0; x < dw && x / 8 < (int)sizeof(row); x++)
            if (dot(pix, w, h, stride, s, x, y))
                row[x / 8] |= (uint8_t)(0x80 >> (x & 7));
        snprintf(t, sizeof(t), "\x1b*b%dW", nb);
        puts_(b, t);
        put(b, row, (uint32_t)nb);
    }
    puts_(b, "\x1b*rB"); /* end raster */
}

/* Text printers: the picture as characters, darker = more ink */
static void image_text(buf_t *b, const uint8_t *pix, int w, int h, int stride) {
    static const char RAMP[] = "@#*+=-:. "; /* dark .. light */
    int cw = (w + PRINT_COLS - 1) / PRINT_COLS;
    if (cw < 1)
        cw = 1;
    int ch = cw * 2; /* a character is about twice as tall as wide */
    int lines = 0;
    for (int y = 0; y < h; y += ch) {
        if (lines == PRINT_LINES) {
            putc_(b, '\f');
            lines = 0;
        }
        for (int x = 0; x < w; x += cw) {
            uint32_t sum = 0, n = 0;
            for (int yy = y; yy < y + ch && yy < h; yy++)
                for (int xx = x; xx < x + cw && xx < w; xx++) {
                    sum += print_grey(pix[yy * stride + xx]);
                    n++;
                }
            putc_(b, (uint8_t)RAMP[n ? sum / n * 8 / 255 : 8]);
        }
        put(b, "\r\n", 2);
        lines++;
    }
}

// queue

bool print_available(void) { return lpt_base(g_cfg.printer_port) != 0; }

const char *print_port_name(void) {
    static char n[8];
    if (!print_available())
        return "no port";
    snprintf(n, sizeof(n), "LPT%d", g_cfg.printer_port + 1);
    return n;
}

static int add_job(const char *name, buf_t *b, int pages, int copies) {
    if (b->fail || !b->len || s_njobs >= PRINT_MAX_JOBS) {
        kfree(b->p);
        return -1;
    }
    if (copies > 1) { /* the same bytes again */
        uint32_t one = b->len;
        for (int c = 1; c < copies && !b->fail; c++)
            put(b, b->p, one);
        if (b->fail) {
            kfree(b->p);
            return -1;
        }
    }
    print_job_t *j = &s_jobs[s_njobs++];
    memset(j, 0, sizeof(*j));
    j->id = s_next_id++;
    strncpy(j->name, name ? name : "Untitled", sizeof(j->name) - 1);
    j->data = b->p;
    j->len = b->len;
    j->pages = pages * (copies > 1 ? copies : 1);
    j->state = PJ_WAITING;
    j->port = g_cfg.printer_port;
    s_rev++;
    return j->id;
}

int print_submit_text(const char *name, const char *text, uint32_t len,
                      int copies, int from, int to) {
    if (!print_available())
        return -1;
    buf_t b = {0};
    int type = g_cfg.printer_type % PRN_TYPES;
    if (type == PRN_ESCP)
        put(&b, "\x1b@", 2);
    else if (type == PRN_PCL)
        puts_(&b, "\x1b"
                  "E");
    int pages = print_text_pages(text, len);
    if (from < 1)
        from = 1;
    if (to < from || to > pages)
        to = pages;
    text_out_t o = {&b, from, to, 0, false};
    layout(text, len, emit_line, &o);
    if (!g_cfg.printer_no_ff || type == PRN_PCL)
        putc_(&b, '\f'); /* eject the last page */
    if (type == PRN_PCL)
        puts_(&b, "\x1b"
                  "E");
    return add_job(name, &b, to - from + 1, copies);
}

int print_submit_image(const char *name, const uint8_t *pix, int w, int h,
                       int stride, int copies) {
    if (!print_available() || !pix || w <= 0 || h <= 0)
        return -1;
    buf_t b = {0};
    int type = g_cfg.printer_type % PRN_TYPES;
    if (type == PRN_ESCP)
        image_escp(&b, pix, w, h, stride);
    else if (type == PRN_PCL)
        image_pcl(&b, pix, w, h, stride);
    else
        image_text(&b, pix, w, h, stride);
    if (!g_cfg.printer_no_ff || type == PRN_PCL)
        putc_(&b, '\f');
    if (type == PRN_PCL)
        puts_(&b, "\x1b"
                  "E");
    return add_job(name, &b, 1, copies);
}

static void drop(int i) {
    kfree(s_jobs[i].data);
    for (int k = i; k < s_njobs - 1; k++)
        s_jobs[k] = s_jobs[k + 1];
    s_njobs--;
    s_rev++;
}

/* A job being printed can be deleted too: the printer then gets a form
 * feed so the next job starts on a new page. */
bool print_delete(int id) {
    for (int i = 0; i < s_njobs; i++)
        if (s_jobs[i].id == id) {
            bool mid = s_jobs[i].sent && s_jobs[i].sent < s_jobs[i].len;
            int port = s_jobs[i].port;
            drop(i);
            if (mid)
                lpt_try_put(port, '\f');
            return true;
        }
    return false;
}

void print_pump(void) {
    if (s_paused || !s_njobs)
        return;
    print_job_t *j = &s_jobs[0];
    uint32_t start = pit_ticks;
    int budget = 4096;
    while (j->sent < j->len && budget-- > 0) {
        lpt_status_t st = lpt_try_put(j->port, j->data[j->sent]);
        if (st != LPT_READY) {
            uint8_t ns = st == LPT_BUSY ? PJ_PRINTING : PJ_ERROR;
            if (st != s_last || ns != j->state)
                s_rev++;
            s_last = st;
            j->state = ns;
            return;
        }
        j->sent++;
        if ((j->sent & 255) == 0)
            s_rev++; /* progress */
        if (pit_ticks - start >= 4)
            break; /* a few ms a frame at most */
    }
    if (s_last != LPT_READY || j->state != PJ_PRINTING)
        s_rev++;
    s_last = LPT_READY;
    j->state = PJ_PRINTING;
    if (j->sent >= j->len)
        drop(0);
}

int print_job_count(void) { return s_njobs; }
const print_job_t *print_job(int i) {
    return i >= 0 && i < s_njobs ? &s_jobs[i] : NULL;
}
void print_set_paused(bool p) {
    s_paused = p;
    s_rev++;
}
bool print_paused(void) { return s_paused; }
uint32_t print_queue_rev(void) { return s_rev; }

const char *print_status_text(void) {
    if (!print_available())
        return "No printer port";
    if (s_paused)
        return "Paused";
    if (!s_njobs) {
        lpt_status_t st = lpt_status(g_cfg.printer_port);
        return st == LPT_READY  ? "Ready"
               : st == LPT_BUSY ? "Busy"
                                : lpt_status_text(st);
    }
    if (s_jobs[0].state == PJ_ERROR)
        return lpt_status_text(s_last);
    return s_jobs[0].sent ? "Printing" : "Waiting for the printer";
}
