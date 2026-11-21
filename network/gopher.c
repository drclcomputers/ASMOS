/* RFC 1436 Gopher client for ASMTerm: menus, text, search, downloads, URLs,
 * history, and navigation. Redraws in place when clear() is supported. */
#include "network/gopher.h"
#include "fs/fs.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "network/dns.h"
#include "network/net.h"
#include "os/scheduler.h"
#include "shell/cli.h"
#include "shell/term_buf.h"

#define GOPHER_PORT 70
#define LINE_MAX 255
#define HOST_MAX 64
#define MAX_ITEMS 200
#define FETCH_BUF_SIZE 32768
#define MAX_ROWS 3000
#define HIST_MAX 32
#define CONNECT_IDLE_MS 8000

typedef struct {
    char type;
    char display[LINE_MAX];
    char selector[LINE_MAX];
    char host[HOST_MAX];
    uint16_t port;
} gopher_item_t;

typedef struct {
    char host[HOST_MAX];
    uint16_t port;
    char type; /* '1' menu, '0' text, ... */
    char sel[LINE_MAX];
    int cursor, scroll, top;
} gopher_loc_t;

typedef struct {
    term_context_t *ctx;
    int cols, rows, page;

    gopher_item_t items[MAX_ITEMS];
    int count;
    gopher_loc_t items_loc; /* which page items[] belongs to */
    bool items_valid;

    char buf[FETCH_BUF_SIZE];
    int len;
    bool truncated;

    uint32_t row_off[MAX_ROWS];
    uint16_t row_len[MAX_ROWS];
    int nrows;

    gopher_loc_t cur;
    gopher_loc_t hist[HIST_MAX];
    int hist_n;

    int cursor, scroll; /* menu view */
    int top;            /* text view */
    char status[96];
} gopher_t;

// small helpers

bool parse_ip_str(const char *str, uint8_t ip[4]) {
    uint8_t parts[4] = {0};
    int part = 0, digits = 0;
    uint16_t val = 0;
    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (val > 255 || ++digits > 3)
                return false;
        } else if (c == '.') {
            if (part >= 3 || digits == 0)
                return false;
            parts[part++] = (uint8_t)val;
            val = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    if (part != 3 || digits == 0)
        return false;
    parts[3] = (uint8_t)val;
    memcpy(ip, parts, 4);
    return true;
}

static void scopy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void out(gopher_t *g, const char *msg) {
    if (g->ctx && g->ctx->print) {
        char b[TERM_BUF_LINE_W + 2];
        int i = 0;
        while (msg[i] && i < TERM_BUF_LINE_W) {
            b[i] = msg[i];
            i++;
        }
        b[i++] = '\n';
        b[i] = 0;
        g->ctx->print(g->ctx, b);
    } else {
        term_buf_push(msg);
    }
}

static void set_status(gopher_t *g, const char *msg) {
    scopy(g->status, msg, sizeof(g->status));
}

static int getkey(gopher_t *g) {
    return (g->ctx && g->ctx->getchar) ? g->ctx->getchar(g->ctx) : 'q';
}

static void read_size(gopher_t *g) {
    g->cols = 76;
    g->rows = 22;
    if (g->ctx && g->ctx->size)
        g->ctx->size(g->ctx, &g->cols, &g->rows);
    if (g->cols > TERM_BUF_LINE_W - 1)
        g->cols = TERM_BUF_LINE_W - 1;
    if (g->cols < 20)
        g->cols = 20;
    g->page = g->rows - 3; /* header, footer, status */
    if (g->page < 4)
        g->page = 4;
}

/* Copy src to dst, at most `cols` printable ASCII columns. UTF-8 sequences
 * become one '?', tabs become spaces, control chars are dropped. */
static int fit(char *dst, const char *src, int srclen, int cols) {
    int o = 0;
    for (int i = 0; (srclen < 0 ? src[i] != 0 : i < srclen) && o < cols; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\t')
            dst[o++] = ' ';
        else if (c >= 0xC0)
            dst[o++] = '?';
        else if (c >= 32 && c < 127)
            dst[o++] = (char)c;
    }
    dst[o] = 0;
    return o;
}

static void loc_url(const gopher_loc_t *l, char *o, int max) {
    char sel[LINE_MAX];
    scopy(sel, l->sel, sizeof(sel));
    for (int i = 0; sel[i]; i++)
        if (sel[i] == '\t')
            sel[i] = '?'; /* search query: show it readably */
    if (l->port == GOPHER_PORT)
        snprintf(o, max, "gopher://%s/%c%s", l->host, l->type, sel);
    else
        snprintf(o, max, "gopher://%s:%u/%c%s", l->host, l->port, l->type, sel);
}

static bool same_loc(const gopher_loc_t *a, const gopher_loc_t *b) {
    return a->port == b->port && a->type == b->type &&
           strcmp(a->host, b->host) == 0 && strcmp(a->sel, b->sel) == 0;
}

/* host[:port][/T[selector]]  or  gopher://...   (%XX decoded) */
static bool parse_location(const char *s, gopher_loc_t *l) {
    memset(l, 0, sizeof(*l));
    while (*s == ' ')
        s++;
    if (strncmp(s, "gopher://", 9) == 0)
        s += 9;
    int h = 0;
    while (*s && *s != ':' && *s != '/' && *s != ' ' && h < HOST_MAX - 1)
        l->host[h++] = *s++;
    l->host[h] = 0;
    if (!h)
        return false;
    l->port = GOPHER_PORT;
    if (*s == ':') {
        s++;
        uint32_t p = 0;
        while (*s >= '0' && *s <= '9')
            p = p * 10 + (uint32_t)(*s++ - '0');
        if (p == 0 || p > 65535)
            return false;
        l->port = (uint16_t)p;
    }
    l->type = '1';
    if (*s == '/' && s[1]) {
        l->type = s[1];
        s += 2;
    } else if (*s == '/') {
        s++;
    }
    int o = 0;
    while (*s && o < LINE_MAX - 1) {
        if (s[0] == '%' && s[1] && s[2]) {
            int v = 0, ok = 1;
            for (int k = 1; k <= 2; k++) {
                char c = s[k];
                v <<= 4;
                if (c >= '0' && c <= '9')
                    v |= c - '0';
                else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
                    v |= (c | 0x20) - 'a' + 10;
                else
                    ok = 0;
            }
            if (ok) {
                l->sel[o++] = (char)v;
                s += 3;
                continue;
            }
        }
        l->sel[o++] = *s++;
    }
    l->sel[o] = 0;
    return true;
}

static bool resolve_host(gopher_t *g, const char *host, uint8_t ip[4]) {
    if (parse_ip_str(host, ip))
        return true;
    char m[96];
    snprintf(m, sizeof(m), "Resolving %s ...", host);
    out(g, m);
    if (dns_resolve(host, ip))
        return true;
    snprintf(m, sizeof(m), "Can't resolve %s (check DNS in Settings)", host);
    set_status(g, m);
    return false;
}

// network

/* Fetch l into g->buf (sink == NULL) or stream it into an open file.
 * Returns bytes received, or -1 on failure (g->status says why). */
static int fetch(gopher_t *g, const gopher_loc_t *l, fat_file_t *sink) {
    uint8_t ip[4];
    if (!resolve_host(g, l->host, ip))
        return -1;

    char m[96];
    snprintf(m, sizeof(m), "Connecting to %s:%u ...", l->host, l->port);
    out(g, m);
    int sock = tcp_connect(ip, l->port, 0);
    if (sock < 0) {
        snprintf(m, sizeof(m), "Can't connect to %s:%u", l->host, l->port);
        set_status(g, m);
        return -1;
    }

    char req[LINE_MAX + 3];
    int rl = 0;
    for (int i = 0; l->sel[i] && rl < LINE_MAX; i++)
        req[rl++] = l->sel[i];
    req[rl++] = '\r';
    req[rl++] = '\n';
    if (!tcp_send(sock, (const uint8_t *)req, (uint16_t)rl)) {
        tcp_close(sock);
        set_status(g, "Send failed");
        return -1;
    }

    int total = 0;
    if (!sink) {
        g->len = 0;
        g->truncated = false;
    }
    uint8_t chunk[1024];
    uint32_t idle_until = time_millis() + CONNECT_IDLE_MS;
    uint32_t next_note = time_millis() + 1000;
    bool failed = false;
    for (;;) {
        net_poll();
        int n = tcp_recv(sock, chunk, sizeof(chunk));
        if (n > 0) {
            idle_until = time_millis() + CONNECT_IDLE_MS;
            if (sink) {
                if (fs_write(sink, chunk, n) != n) {
                    set_status(g, "Disk full / write error");
                    failed = true;
                    break;
                }
            } else {
                int room = FETCH_BUF_SIZE - g->len;
                if (n > room) {
                    memcpy(g->buf + g->len, chunk, room);
                    g->len += room;
                    g->truncated = true;
                    total += room;
                    break; /* page buffer full: stop downloading */
                }
                memcpy(g->buf + g->len, chunk, n);
                g->len += n;
            }
            total += n;
            if (sink && time_millis() >= next_note) {
                snprintf(m, sizeof(m), "  %d KB ...", total / 1024);
                out(g, m);
                next_note = time_millis() + 1000;
            }
            continue;
        }
        tcp_state_t st = tcp_state(sock);
        if (st != TCP_ESTABLISHED) /* server closed: done */
            break;
        if (time_millis() >= idle_until) {
            if (total == 0) {
                set_status(g, "Timed out waiting for the server");
                failed = true;
            }
            break;
        }
        task_yield();
    }
    /* drain anything that arrived with the FIN */
    if (!failed && !g->truncated) {
        int n;
        while ((n = tcp_recv(sock, chunk, sizeof(chunk))) > 0) {
            if (sink)
                fs_write(sink, chunk, n);
            else {
                int room = FETCH_BUF_SIZE - g->len;
                if (n > room) {
                    n = room;
                    g->truncated = true;
                }
                memcpy(g->buf + g->len, chunk, n);
                g->len += n;
            }
            total += n;
        }
    }
    tcp_close(sock);
    return failed ? -1 : total;
}

// parsing

static bool selectable(char t) { return t != 'i' && t != '3' && t != 0; }

static void parse_menu(gopher_t *g, const gopher_loc_t *from) {
    g->count = 0;
    int i = 0;
    while (i < g->len && g->count < MAX_ITEMS) {
        int start = i;
        while (i < g->len && g->buf[i] != '\n')
            i++;
        int end = i;
        if (i < g->len)
            i++;
        if (end > start && g->buf[end - 1] == '\r')
            end--;
        if (end - start == 1 && g->buf[start] == '.')
            break; /* end of menu */
        if (end == start)
            continue;

        gopher_item_t *it = &g->items[g->count];
        memset(it, 0, sizeof(*it));
        it->type = g->buf[start];
        int field = 0, fi = 0;
        char portbuf[8] = {0};
        for (int k = start + 1; k < end; k++) {
            char c = g->buf[k];
            if (c == '\t') {
                field++;
                fi = 0;
                continue;
            }
            switch (field) {
            case 0:
                if (fi < LINE_MAX - 1)
                    it->display[fi++] = c;
                break;
            case 1:
                if (fi < LINE_MAX - 1)
                    it->selector[fi++] = c;
                break;
            case 2:
                if (fi < HOST_MAX - 1)
                    it->host[fi++] = c;
                break;
            case 3:
                if (fi < 7)
                    portbuf[fi++] = c;
                break;
            }
        }
        uint32_t port = 0;
        for (int k = 0; portbuf[k] >= '0' && portbuf[k] <= '9'; k++)
            port = port * 10 + (uint32_t)(portbuf[k] - '0');
        it->port = (port && port < 65536) ? (uint16_t)port : GOPHER_PORT;
        /* missing host (sloppy servers): same server as the menu */
        if (!it->host[0] || strcmp(it->host, "(NULL)") == 0) {
            scopy(it->host, from->host, HOST_MAX);
            if (!port)
                it->port = from->port;
        }
        if (field == 0 && it->type != 'i' && it->type != '3') {
            /* no tabs at all: plain text in a menu, show as info */
            it->display[0] = it->type;
            fit(it->display + 1, g->buf + start + 1, end - start - 1,
                LINE_MAX - 2);
            it->type = 'i';
        }
        g->count++;
    }
    g->items_loc = *from;
    g->items_valid = true;
}

/* Split g->buf into display rows wrapped at g->cols. */
static void layout_text(gopher_t *g) {
    g->nrows = 0;
    int i = 0;
    while (i < g->len && g->nrows < MAX_ROWS) {
        int start = i;
        while (i < g->len && g->buf[i] != '\n')
            i++;
        int end = i;
        if (i < g->len)
            i++;
        if (end > start && g->buf[end - 1] == '\r')
            end--;
        if (end - start == 1 && g->buf[start] == '.')
            break; /* end of document */
        if (end - start >= 2 && g->buf[start] == '.' &&
            g->buf[start + 1] == '.')
            start++; /* dot-unstuffing */
        if (end == start) {
            g->row_off[g->nrows] = (uint32_t)start;
            g->row_len[g->nrows++] = 0;
            continue;
        }
        while (start < end && g->nrows < MAX_ROWS) {
            int n = end - start;
            if (n > g->cols) {
                n = g->cols;
                /* break at the last space if there is one */
                for (int k = n; k > g->cols / 2; k--)
                    if (g->buf[start + k] == ' ') {
                        n = k + 1;
                        break;
                    }
            }
            g->row_off[g->nrows] = (uint32_t)start;
            g->row_len[g->nrows++] = (uint16_t)n;
            start += n;
        }
    }
}

// drawing

static const char *type_label(char t, const char *sel) {
    switch (t) {
    case '1':
        return "DIR ";
    case '0':
        return "TXT ";
    case '7':
        return "FIND";
    case 'h':
        return strncmp(sel, "URL:", 4) == 0 ? "WEB " : "HTML";
    case '9':
        return "BIN ";
    case '4':
    case '6':
        return "ENC ";
    case '5':
        return "ZIP ";
    case 'g':
    case 'I':
    case 'p':
        return "IMG ";
    case 's':
        return "SND ";
    case ';':
        return "VID ";
    case 'd':
        return "DOC ";
    case '8':
    case 'T':
        return "TEL ";
    case '2':
        return "CSO ";
    case '3':
        return "ERR!";
    case 'i':
        return "    ";
    default:
        return "??? ";
    }
}

static void header(gopher_t *g) {
    char url[TERM_BUF_LINE_W * 2], line[TERM_BUF_LINE_W];
    loc_url(&g->cur, url, sizeof(url));
    fit(line, url, -1, g->cols);
    if (g->ctx && g->ctx->clear)
        g->ctx->clear(g->ctx);
    out(g, line);
}

static void footer(gopher_t *g, const char *keys) {
    char line[TERM_BUF_LINE_W];
    fit(line, g->status[0] ? g->status : keys, -1, g->cols);
    out(g, line);
    g->status[0] = 0;
}

static void draw_menu(gopher_t *g) {
    read_size(g);
    header(g);
    if (g->cursor < g->scroll)
        g->scroll = g->cursor;
    if (g->cursor >= g->scroll + g->page)
        g->scroll = g->cursor - g->page + 1;
    if (g->scroll < 0)
        g->scroll = 0;
    char line[TERM_BUF_LINE_W];
    int shown = 0;
    for (int i = g->scroll; i < g->count && shown < g->page; i++, shown++) {
        gopher_item_t *it = &g->items[i];
        int p = 0;
        line[p++] = (i == g->cursor) ? '>' : ' ';
        const char *lab = type_label(it->type, it->selector);
        for (int k = 0; lab[k]; k++)
            line[p++] = lab[k];
        line[p++] = ' ';
        fit(line + p, it->display, -1, g->cols - p);
        out(g, line);
    }
    for (; shown < g->page && g->ctx && g->ctx->clear; shown++)
        out(g, "");
    char keys[96];
    snprintf(keys, sizeof(keys),
             "[%d/%d%s] Enter open  b back  d save  g go  u up  ? help",
             g->count ? g->cursor + 1 : 0, g->count,
             g->truncated ? " cut" : "");
    footer(g, keys);
}

static void draw_text(gopher_t *g) {
    read_size(g);
    header(g);
    int maxtop = g->nrows - g->page;
    if (g->top > maxtop)
        g->top = maxtop;
    if (g->top < 0)
        g->top = 0;
    char line[TERM_BUF_LINE_W];
    int shown = 0;
    for (int r = g->top; r < g->nrows && shown < g->page; r++, shown++) {
        fit(line, g->buf + g->row_off[r], g->row_len[r], g->cols);
        out(g, line);
    }
    for (; shown < g->page && g->ctx && g->ctx->clear; shown++)
        out(g, "");
    int end = g->top + g->page;
    if (end > g->nrows)
        end = g->nrows;
    int pct = g->nrows ? end * 100 / g->nrows : 100;
    char keys[96];
    snprintf(keys, sizeof(keys),
             "-- %d%%%s -- Space/PgDn more  b back  d save  q quit", pct,
             g->truncated ? " (cut at 32 KB, d saves all)" : "");
    footer(g, keys);
}

static void draw(gopher_t *g) {
    if (g->cur.type == '1' || g->cur.type == '7')
        draw_menu(g);
    else
        draw_text(g);
}

static void help(gopher_t *g) {
    read_size(g);
    header(g);
    static const char *const lines[] = {
        "Keys",
        "  Up/Down, j/k       move        PgUp/PgDn, Space  page",
        "  Home/End           first/last  Enter, Right, l   open",
        "  b, Left, Bksp, Esc back        q                 quit",
        "  d                  save selected item (or this page) to disk",
        "  g                  go to a gopher:// URL or host[:port]",
        "  u                  up one level  r  reload  ? this help",
        "",
        "Text pages open in the viewer, files are saved to /GOPHER/<host>/.",
        "Search items (FIND) ask for a query.",
        "",
        "Press any key...",
        NULL};
    for (int i = 0; lines[i]; i++) {
        char line[TERM_BUF_LINE_W];
        fit(line, lines[i], -1, g->cols);
        out(g, line);
    }
    getkey(g);
}

// navigation

static void make_save_path(const gopher_loc_t *l, char *outp, int maxlen) {
    char host8[9];
    int hi = 0;
    for (int i = 0; l->host[i] && hi < 8; i++) {
        char c = l->host[i];
        if (c == '.' && hi > 0 && !(l->host[0] >= '0' && l->host[0] <= '9'))
            break; /* first label of a name; IPs keep their digits */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
            host8[hi++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    if (!hi)
        host8[hi++] = 'G';
    host8[hi] = 0;

    const char *base = l->sel;
    for (int i = 0; l->sel[i]; i++)
        if (l->sel[i] == '/')
            base = l->sel + i + 1;
    char name[9], ext[4];
    int ni = 0, ei = 0;
    const char *dot = strrchr(base, '.');
    for (int i = 0; base[i] && base + i != dot && ni < 8; i++) {
        char c = base[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            name[ni++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    if (!ni) {
        scopy(name, "INDEX", sizeof(name));
        ni = 5;
    }
    name[ni] = 0;
    if (dot)
        for (int i = 1; dot[i] && ei < 3; i++) {
            char c = dot[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9'))
                ext[ei++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
    ext[ei] = 0;
    if (!ei) {
        const char *e = "BIN";
        switch (l->type) {
        case '0':
        case '1':
        case '7':
            e = "TXT";
            break;
        case 'h':
            e = "HTM";
            break;
        case '5':
            e = "ZIP";
            break;
        case '6':
            e = "UUE";
            break;
        case 'g':
            e = "GIF";
            break;
        case 'I':
        case 'p':
            e = "IMG";
            break;
        }
        scopy(ext, e, sizeof(ext));
    }

    dir_entry_t de;
    char dir[24];
    snprintf(dir, sizeof(dir), "/GOPHER/%s", host8);
    if (!fs_find("/GOPHER", &de))
        fs_mkdir("/GOPHER");
    if (!fs_find(dir, &de))
        fs_mkdir(dir);
    snprintf(outp, maxlen, "%s/%s.%s", dir, name, ext);
}

static void download(gopher_t *g, const gopher_loc_t *l) {
    char path[48], m[96];
    make_save_path(l, path, sizeof(path));
    dir_entry_t de;
    if (fs_find(path, &de))
        fs_delete(path);
    fat_file_t f;
    if (!fs_create(path, &f)) {
        snprintf(m, sizeof(m), "Can't create %s", path);
        set_status(g, m);
        return;
    }
    snprintf(m, sizeof(m), "Saving to %s ...", path);
    out(g, m);
    int n = fetch(g, l, &f);
    fs_close(&f);
    if (n < 0) {
        fs_delete(path);
        return; /* status already set */
    }
    snprintf(m, sizeof(m), "Saved %d bytes to %s", n, path);
    set_status(g, m);
}

static void hist_push(gopher_t *g) {
    if (g->hist_n == HIST_MAX) {
        memmove(&g->hist[0], &g->hist[1], sizeof(g->hist[0]) * (HIST_MAX - 1));
        g->hist_n--;
    }
    g->cur.cursor = g->cursor;
    g->cur.scroll = g->scroll;
    g->cur.top = g->top;
    g->hist[g->hist_n++] = g->cur;
}

static int first_selectable(gopher_t *g) {
    for (int i = 0; i < g->count; i++)
        if (selectable(g->items[i].type))
            return i;
    return 0;
}

/* Load l and make it current. push: remember the page we're leaving. */
static bool go(gopher_t *g, const gopher_loc_t *l, bool push) {
    bool menu = (l->type == '1' || l->type == '7');
    bool cached = menu && g->items_valid && same_loc(&g->items_loc, l);
    if (!cached) {
        if (fetch(g, l, NULL) < 0)
            return false;
        if (menu)
            parse_menu(g, l);
        else
            layout_text(g);
    }
    if (push && g->cur.host[0])
        hist_push(g);
    g->cur = *l;
    if (menu) {
        g->cursor = cached ? l->cursor : first_selectable(g);
        g->scroll = cached ? l->scroll : 0;
    } else {
        g->top = 0;
    }
    return true;
}

static void back(gopher_t *g) {
    if (!g->hist_n) {
        set_status(g, "No history (q quits)");
        return;
    }
    gopher_loc_t l = g->hist[g->hist_n - 1];
    if (go(g, &l, false)) {
        g->hist_n--;
        g->cursor = l.cursor;
        g->scroll = l.scroll;
        g->top = l.top;
    }
}

static bool prompt(gopher_t *g, const char *label, char *buf, int max) {
    out(g, label);
    if (!g->ctx || !g->ctx->readline)
        return false;
    int n = g->ctx->readline(g->ctx, buf, max);
    while (n > 0 && buf[n - 1] == ' ')
        buf[--n] = 0;
    return n > 0;
}

static void open_item(gopher_t *g, gopher_item_t *it) {
    gopher_loc_t l;
    memset(&l, 0, sizeof(l));
    scopy(l.host, it->host, HOST_MAX);
    l.port = it->port;
    l.type = it->type;
    scopy(l.sel, it->selector, LINE_MAX);
    char m[TERM_BUF_LINE_W];

    switch (it->type) {
    case '1':
    case '0':
        go(g, &l, true);
        return;
    case '7': {
        char q[96];
        if (!prompt(g, "Search for (Enter to cancel):", q, sizeof(q)))
            return;
        int n = (int)strlen(l.sel);
        snprintf(l.sel + n, LINE_MAX - n, "\t%s", q);
        go(g, &l, true);
        return;
    }
    case 'h':
        if (strncmp(it->selector, "URL:", 4) == 0) {
            snprintf(m, sizeof(m), "Web link (no browser): %s",
                     it->selector + 4);
            set_status(g, m);
        } else {
            l.type = '0'; /* show raw HTML as text */
            go(g, &l, true);
        }
        return;
    case '8':
    case 'T':
    case '2':
        set_status(g, "Telnet/CSO items aren't supported");
        return;
    case 'i':
    case '3':
        return;
    default:
        download(g, &l); /* binary, images, archives, unknown types */
        return;
    }
}

static int step(gopher_t *g, int from, int dir) {
    for (int p = from + dir; p >= 0 && p < g->count; p += dir)
        if (selectable(g->items[p].type))
            return p;
    return from;
}

static void up_level(gopher_t *g) {
    gopher_loc_t l = g->cur;
    char *t = strchr(l.sel, '\t');
    if (t) {
        *t = 0; /* search results -> search item's server root */
    }
    int n = (int)strlen(l.sel);
    while (n > 0 && l.sel[n - 1] == '/')
        l.sel[--n] = 0;
    while (n > 0 && l.sel[n - 1] != '/')
        l.sel[--n] = 0;
    l.type = '1';
    if (same_loc(&l, &g->cur)) {
        set_status(g, "Already at the top");
        return;
    }
    go(g, &l, true);
}

// session

static void session(gopher_t *g, const gopher_loc_t *start) {
    if (g->ctx)
        asmterm_set_gopher_mode(g->ctx, true);
    if (!go(g, start, false)) {
        out(g, g->status[0] ? g->status : "Gopher: fetch failed");
        if (g->ctx)
            asmterm_set_gopher_mode(g->ctx, false);
        return;
    }
    for (;;) {
        draw(g);
        int c = getkey(g);
        bool menu = (g->cur.type == '1' || g->cur.type == '7');

        if (c == 'q' || c == 'Q')
            break;
        if (c == TERM_KEY_ESC && !g->hist_n)
            break;
        if (c == 'b' || c == 'B' || c == TERM_KEY_LEFT || c == '\b' ||
            c == TERM_KEY_ESC) {
            back(g);
        } else if (c == '?') {
            help(g);
        } else if (c == 'r' || c == 'R') {
            g->items_valid = false;
            gopher_loc_t l = g->cur;
            l.cursor = g->cursor;
            l.scroll = g->scroll;
            if (go(g, &l, false)) {
                g->cursor = l.cursor < g->count ? l.cursor : 0;
                g->scroll = l.scroll;
            }
        } else if (c == 'g' || c == 'G') {
            char s[160];
            gopher_loc_t l;
            if (prompt(g, "Go to (gopher://host[:port]/1/path or host):", s,
                       sizeof(s))) {
                if (parse_location(s, &l))
                    go(g, &l, true);
                else
                    set_status(g, "Bad address");
            }
        } else if (c == 'u' || c == 'U') {
            up_level(g);
        } else if (menu) {
            if (c == TERM_KEY_UP || c == 'k' || c == 'K')
                g->cursor = step(g, g->cursor, -1);
            else if (c == TERM_KEY_DOWN || c == 'j' || c == 'J')
                g->cursor = step(g, g->cursor, 1);
            else if (c == TERM_KEY_PGUP) {
                g->cursor -= g->page;
                if (g->cursor < 0)
                    g->cursor = 0;
            } else if (c == TERM_KEY_PGDN || c == ' ') {
                g->cursor += g->page;
                if (g->cursor >= g->count)
                    g->cursor = g->count - 1;
            } else if (c == TERM_KEY_HOME)
                g->cursor = first_selectable(g);
            else if (c == TERM_KEY_END)
                g->cursor = g->count ? g->count - 1 : 0;
            else if ((c == '\n' || c == TERM_KEY_RIGHT || c == 'l') &&
                     g->cursor < g->count)
                open_item(g, &g->items[g->cursor]);
            else if ((c == 'd' || c == 'D') && g->cursor < g->count) {
                gopher_item_t *it = &g->items[g->cursor];
                if (selectable(it->type) && it->type != '7' &&
                    it->type != '8' && it->type != 'T') {
                    gopher_loc_t l;
                    memset(&l, 0, sizeof(l));
                    scopy(l.host, it->host, HOST_MAX);
                    l.port = it->port;
                    l.type = it->type;
                    scopy(l.sel, it->selector, LINE_MAX);
                    download(g, &l);
                }
            }
        } else { /* text viewer */
            if (c == TERM_KEY_UP || c == 'k' || c == 'K')
                g->top--;
            else if (c == TERM_KEY_DOWN || c == 'j' || c == 'J' || c == '\n')
                g->top++;
            else if (c == TERM_KEY_PGUP)
                g->top -= g->page;
            else if (c == TERM_KEY_PGDN || c == ' ')
                g->top += g->page;
            else if (c == TERM_KEY_HOME)
                g->top = 0;
            else if (c == TERM_KEY_END)
                g->top = g->nrows;
            else if (c == 'd' || c == 'D')
                download(g, &g->cur);
        }
    }
    if (g->ctx && g->ctx->clear)
        g->ctx->clear(g->ctx);
    out(g, "Gopher: closed");
    if (g->ctx)
        asmterm_set_gopher_mode(g->ctx, false);
}

static void run(term_context_t *ctx, const gopher_loc_t *start) {
    gopher_t *g = (gopher_t *)kzalloc(sizeof(gopher_t));
    if (!g) {
        if (ctx && ctx->print)
            ctx->print(ctx, "Gopher: out of memory\n");
        return;
    }
    g->ctx = ctx;
    session(g, start);
    kfree(g);
}

void gopher_run(term_context_t *ctx, const uint8_t start_ip[4], uint16_t port,
                const char *selector) {
    gopher_loc_t l;
    memset(&l, 0, sizeof(l));
    snprintf(l.host, HOST_MAX, "%u.%u.%u.%u", start_ip[0], start_ip[1],
             start_ip[2], start_ip[3]);
    l.port = port ? port : GOPHER_PORT;
    l.type = '1';
    scopy(l.sel, selector ? selector : "", LINE_MAX);
    run(ctx, &l);
}

/* host may also be a full gopher:// URL (then port/selector are ignored). */
void gopher_run_host(term_context_t *ctx, const char *host, uint16_t port,
                     const char *selector) {
    gopher_loc_t l;
    if (!parse_location(host, &l)) {
        if (ctx && ctx->print)
            ctx->print(ctx, "Gopher: bad address\n");
        return;
    }
    bool is_url = strncmp(host, "gopher://", 9) == 0 || strchr(host, '/');
    if (!is_url) {
        if (port && !strchr(host, ':'))
            l.port = port;
        scopy(l.sel, selector ? selector : "", LINE_MAX);
    }
    run(ctx, &l);
}
