#include "network/gopher.h"
#include "fs/fs.h"
#include "io/keyboard.h"
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
#define GOPHER_LOCAL 49152
#define LINE_MAX 255
#define MAX_LINES 200
#define FETCH_BUF_SIZE 32768
#define HIST_MAX 16
#define PAGE_SIZE 18

typedef struct {
    char type;
    char display[LINE_MAX];
    char selector[LINE_MAX];
    uint8_t host[4];
    uint16_t port;
    bool is_link;
    bool is_file;
    char hostname[64];
} gopher_line_t;

typedef struct {
    uint8_t ip[4];
    uint16_t port;
    char sel[LINE_MAX];
    int cursor;
    int scroll;
} hist_entry_t;

typedef struct {
    gopher_line_t lines[MAX_LINES];
    char fetch_buf[FETCH_BUF_SIZE];
    hist_entry_t history[HIST_MAX];
    int line_count;
    int cursor;
    int scroll;
    int hist_top;
} gopher_ctx_t;

bool parse_ip_str(const char *str, uint8_t ip[4]) {
    uint8_t parts[4] = {0};
    int part = 0;
    uint16_t val = 0;
    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
        } else if (c == '.') {
            if (part >= 3)
                return false;
            parts[part++] = (uint8_t)val;
            val = 0;
        } else {
            return false;
        }
    }
    if (part != 3)
        return false;
    parts[3] = (uint8_t)val;
    memcpy(ip, parts, 4);
    return true;
}

static void gopher_print(term_context_t *ctx, const char *msg) {
    if (ctx && ctx->print) {
        char buf[TERM_BUF_LINE_W + 2];
        int i = 0;
        while (msg[i] && i < TERM_BUF_LINE_W) {
            buf[i] = msg[i];
            i++;
        }
        buf[i++] = '\n';
        buf[i] = '\0';
        ctx->print(ctx, buf);
    } else {
        term_buf_push(msg);
    }
}

static int gopher_strncpy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i;
}

static void parse_response(gopher_ctx_t *g, int len) {
    const char *buf = g->fetch_buf;
    g->line_count = 0;
    int i = 0;
    while (i < len && g->line_count < MAX_LINES) {
        if (buf[i] == '.' &&
            (i + 1 >= len || buf[i + 1] == '\r' || buf[i + 1] == '\n'))
            break;

        char type = buf[i++];
        gopher_line_t *gl = &g->lines[g->line_count];
        gl->type = type;
        gl->is_link = false;
        gl->is_file = false;
        memset(gl->display, 0, LINE_MAX);
        memset(gl->selector, 0, LINE_MAX);
        memset(gl->host, 0, 4);
        memset(gl->hostname, 0, 64);
        gl->port = 0;

        int field = 0, fi = 0;
        char hostbuf[64];
        hostbuf[0] = 0;

        while (i < len && buf[i] != '\n') {
            char c = buf[i++];
            if (c == '\r')
                continue;
            if (c == '\t') {
                field++;
                fi = 0;
                if (field == 2)
                    hostbuf[0] = 0;
                continue;
            }
            switch (field) {
            case 0:
                if (fi < LINE_MAX - 1)
                    gl->display[fi++] = c;
                break;
            case 1:
                if (fi < LINE_MAX - 1)
                    gl->selector[fi++] = c;
                break;
            case 2:
                if (fi < 63) {
                    hostbuf[fi++] = c;
                    hostbuf[fi] = 0;
                }
                if (buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r') {
                    gopher_strncpy(gl->hostname, hostbuf, 64);
                    if (!parse_ip_str(hostbuf, gl->host))
                        memset(gl->host, 0, 4);
                }
                break;
            case 3:
                if (c >= '0' && c <= '9')
                    gl->port = gl->port * 10 + (c - '0');
                break;
            }
        }
        if (i < len)
            i++;

        if (type == '1' || type == '7')
            gl->is_link = true;
        if (type == '0' || type == '4' || type == '5' || type == '6' ||
            type == '9' || type == 'g' || type == 'I')
            gl->is_file = true;

        g->line_count++;
        if (gl->port == 0)
            gl->port = 70;
    }
}

static bool gopher_fetch(gopher_ctx_t *g, const uint8_t ip[4], uint16_t port,
                         const char *selector, int *out_len) {
    int sock = tcp_connect(ip, port, GOPHER_LOCAL);
    if (sock < 0)
        return false;

    char req[LINE_MAX + 4];
    int rlen = 0;
    for (int i = 0; selector[i]; i++)
        req[rlen++] = selector[i];
    req[rlen++] = '\r';
    req[rlen++] = '\n';

    if (!tcp_send(sock, (uint8_t *)req, (uint16_t)rlen)) {
        tcp_close(sock);
        return false;
    }

    int total = 0;
    uint32_t deadline = time_millis() + 5000;
    while (time_millis() < deadline) {
        net_poll();
        task_yield();

        int n;
        while (total < FETCH_BUF_SIZE) {
            n = tcp_recv(sock, (uint8_t *)g->fetch_buf + total,
                         FETCH_BUF_SIZE - total);
            if (n <= 0)
                break;
            total += n;
            deadline = time_millis() + 1000;
        }

        if (total >= FETCH_BUF_SIZE)
            break;

        tcp_state_t st = tcp_state(sock);
        if (st == TCP_CLOSE_WAIT || st == TCP_CLOSED || st == TCP_LAST_ACK) {
            while (total < FETCH_BUF_SIZE) {
                n = tcp_recv(sock, (uint8_t *)g->fetch_buf + total,
                             FETCH_BUF_SIZE - total);
                if (n <= 0)
                    break;
                total += n;
            }
            break;
        }
    }
    tcp_close(sock);
    uint32_t drain_end = time_millis() + 100;
    while (time_millis() < drain_end)
        net_poll();
    *out_len = total;
    return total > 0;
}

static void ip_to_str(const uint8_t ip[4], char *out) {
    int p = 0;
    for (int octet = 0; octet < 4; octet++) {
        uint8_t v = ip[octet];
        if (v >= 100)
            out[p++] = '0' + v / 100;
        if (v >= 10)
            out[p++] = '0' + (v / 10) % 10;
        out[p++] = '0' + v % 10;
        if (octet < 3)
            out[p++] = '.';
    }
    out[p] = '\0';
}

/* Resolve gl->host: use cur_ip if gl->host is zero and no hostname, or DNS
   if gl->hostname is set. Returns false only on DNS failure. */
static bool resolve_gl(gopher_line_t *gl, const uint8_t cur_ip[4],
                       term_context_t *ctx) {
    uint8_t zero[4] = {0, 0, 0, 0};
    if (memcmp(gl->host, zero, 4) != 0)
        return true;

    if (gl->hostname[0]) {
        gopher_print(ctx, "Gopher: resolving...");
        if (!dns_resolve(gl->hostname, gl->host)) {
            gopher_print(ctx, "Gopher: DNS failed");
            return false;
        }
        return true;
    }

    /* No hostname and no IP — item is on the same server as cur_ip */
    memcpy(gl->host, cur_ip, 4);
    return true;
}

static void make_save_path(const char *hostname, const char *selector,
                           char type, char *out, int maxlen) {
    char host83[12];
    int hi = 0;
    for (int i = 0; hostname[i] && hi < 8; i++) {
        char c = hostname[i];
        if (c == '.')
            break;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
            host83[hi++] = (c >= 'a') ? c - 32 : c;
    }
    host83[hi] = '\0';
    if (hi == 0) {
        host83[0] = 'G';
        host83[1] = 'O';
        host83[2] = 'P';
        host83[3] = '\0';
    }

    char base[9];
    int bi = 0;
    const char *sl = selector;
    for (int i = 0; selector[i]; i++)
        if (selector[i] == '/')
            sl = selector + i + 1;
    for (int i = 0; sl[i] && sl[i] != '.' && bi < 8; i++) {
        char c = sl[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
            base[bi++] = (c >= 'a') ? c - 32 : c;
    }
    if (bi == 0) {
        base[0] = 'F';
        base[1] = 'I';
        base[2] = 'L';
        base[3] = 'E';
        bi = 4;
    }
    base[bi] = '\0';

    const char *ext = "TXT";
    if (type == '9' || type == '4')
        ext = "BIN";
    else if (type == '5')
        ext = "ZIP";
    else if (type == '6')
        ext = "UUE";
    else if (type == 'g')
        ext = "GIF";
    else if (type == 'I')
        ext = "IMG";

    char dirpath[64];
    sprintf(dirpath, "/GOPHER/%s", host83);

    dir_entry_t de;
    if (!fs_find("/GOPHER", &de))
        fs_mkdir("/GOPHER");
    if (!fs_find(dirpath, &de))
        fs_mkdir(dirpath);

    snprintf(out, maxlen, "%s/%s.%s", dirpath, base, ext);
}

static void download_file(gopher_ctx_t *g, term_context_t *ctx,
                          const uint8_t ip[4], uint16_t port,
                          const char *selector, char type,
                          const char *hostname) {
    char path[80];
    make_save_path(hostname, selector, type, path, 80);

    char msg[TERM_BUF_LINE_W];
    sprintf(msg, "Saving -> %s", path);
    gopher_print(ctx, msg);

    int fetch_len = 0;
    if (!gopher_fetch(g, ip, port, selector, &fetch_len)) {
        gopher_print(ctx, "Gopher: fetch failed");
        return;
    }

    dir_entry_t de;
    if (fs_find(path, &de))
        fs_delete(path);

    fat_file_t f;
    if (!fs_create(path, &f)) {
        gopher_print(ctx, "Gopher: create failed");
        return;
    }
    int written = fs_write(&f, g->fetch_buf, fetch_len);
    fs_close(&f);

    sprintf(msg, "Saved %d bytes", written);
    gopher_print(ctx, msg);
}

static void print_page(gopher_ctx_t *g, term_context_t *ctx) {
    char line[TERM_BUF_LINE_W];

    gopher_print(ctx, "UP/DOWN:move  ENTER:open  d:save  b:back  q:quit");

    int end = g->scroll + PAGE_SIZE;
    if (end > g->line_count)
        end = g->line_count;

    for (int i = g->scroll; i < end; i++) {
        gopher_line_t *gl = &g->lines[i];

        const char *prefix;
        if (gl->type == '1')
            prefix = "[DIR]";
        else if (gl->type == '0')
            prefix = "[TXT]";
        else if (gl->type == '7')
            prefix = "[QRY]";
        else if (gl->type == '9')
            prefix = "[BIN]";
        else if (gl->type == '4')
            prefix = "[BIN]";
        else if (gl->type == '5')
            prefix = "[ZIP]";
        else if (gl->type == '6')
            prefix = "[UUE]";
        else if (gl->type == 'g')
            prefix = "[GIF]";
        else if (gl->type == 'I')
            prefix = "[IMG]";
        else if (gl->type == 'i')
            prefix = "     ";
        else if (gl->type == '3')
            prefix = "[ERR]";
        else
            prefix = "     ";

        const char *curs = (i == g->cursor) ? ">" : " ";

        int p = 0;
        line[p++] = curs[0];
        line[p++] = ' ';
        for (int j = 0; prefix[j] && p < TERM_BUF_LINE_W - 2; j++)
            line[p++] = prefix[j];
        line[p++] = ' ';
        for (int j = 0; gl->display[j] && p < TERM_BUF_LINE_W - 1; j++)
            line[p++] = gl->display[j];
        line[p] = 0;
        gopher_print(ctx, line);
    }

    char status[48];
    sprintf(status, "  [%d/%d]", g->cursor + 1, g->line_count);
    gopher_print(ctx, status);
    gopher_print(ctx, "");
}

static void hist_push(gopher_ctx_t *g, const uint8_t ip[4], uint16_t port,
                      const char *sel, int cursor, int scroll) {
    if (g->hist_top < HIST_MAX) {
        memcpy(g->history[g->hist_top].ip, ip, 4);
        g->history[g->hist_top].port = port;
        g->history[g->hist_top].cursor = cursor;
        g->history[g->hist_top].scroll = scroll;
        gopher_strncpy(g->history[g->hist_top].sel, sel, LINE_MAX);
        g->hist_top++;
    }
}

static bool hist_pop(gopher_ctx_t *g, uint8_t ip[4], uint16_t *port, char *sel,
                     int *cursor, int *scroll) {
    if (g->hist_top <= 1)
        return false;
    g->hist_top--;
    int idx = g->hist_top - 1;
    memcpy(ip, g->history[idx].ip, 4);
    *port = g->history[idx].port;
    *cursor = g->history[idx].cursor;
    *scroll = g->history[idx].scroll;
    gopher_strncpy(sel, g->history[idx].sel, LINE_MAX);
    return true;
}

static void cursor_set(gopher_ctx_t *g, int pos) {
    if (pos < 0)
        pos = 0;
    if (pos >= g->line_count)
        pos = g->line_count - 1;
    g->cursor = pos;
    if (g->cursor < g->scroll)
        g->scroll = g->cursor;
    if (g->cursor >= g->scroll + PAGE_SIZE)
        g->scroll = g->cursor - PAGE_SIZE + 1;
    if (g->scroll < 0)
        g->scroll = 0;
}

/* Skip to next/prev selectable line (is_link or is_file), wrapping not done */
static int next_selectable(gopher_ctx_t *g, int from, int dir) {
    int pos = from + dir;
    while (pos >= 0 && pos < g->line_count) {
        if (g->lines[pos].is_link || g->lines[pos].is_file)
            return pos;
        pos += dir;
    }
    return from;
}

static void gopher_session(term_context_t *ctx, gopher_ctx_t *g,
                           const uint8_t start_ip[4], uint16_t port,
                           const char *selector) {
    if (ctx)
        asmterm_set_gopher_mode(ctx, true);

    int fetch_len = 0;
    uint8_t cur_ip[4];
    uint16_t cur_port = port;
    char cur_sel[LINE_MAX];
    char cur_host[64];

    memcpy(cur_ip, start_ip, 4);
    gopher_strncpy(cur_sel, selector, LINE_MAX);
    /* Pre-fill cur_host with the dotted IP so downloads always have a dir name
     */
    ip_to_str(start_ip, cur_host);

    g->hist_top = 0;
    g->cursor = 0;
    g->scroll = 0;

    gopher_print(ctx, "Gopher: connecting...");

    if (!gopher_fetch(g, cur_ip, cur_port, cur_sel, &fetch_len)) {
        gopher_print(ctx, "Gopher: fetch failed (no data received)");
        if (ctx)
            asmterm_set_gopher_mode(ctx, false);
        return;
    }

    {
        char dbg[48];
        sprintf(dbg, "Gopher: got %d bytes", fetch_len);
        gopher_print(ctx, dbg);
    }

    hist_push(g, cur_ip, cur_port, cur_sel, 0, 0);
    parse_response(g, fetch_len);

    {
        char dbg[48];
        sprintf(dbg, "Gopher: parsed %d lines", g->line_count);
        gopher_print(ctx, dbg);
    }

    /* Start cursor on first selectable item */
    g->cursor = 0;
    g->scroll = 0;
    {
        int first = next_selectable(g, -1, 1);
        if (first != -1)
            cursor_set(g, first);
    }
    print_page(g, ctx);

    while (1) {
        int c = ctx->getchar(ctx);

        /* Arrow keys arrive as raw PS/2 scancodes from keyboard.h */
        if (c == UP_ARROW || c == 'k' || c == 'K') {
            cursor_set(g, next_selectable(g, g->cursor, -1));
            print_page(g, ctx);

        } else if (c == DOWN_ARROW || c == 'j' || c == 'J') {
            cursor_set(g, next_selectable(g, g->cursor, 1));
            print_page(g, ctx);

        } else if (c == PAGE_UP) {
            cursor_set(g, g->cursor - PAGE_SIZE);
            print_page(g, ctx);

        } else if (c == PAGE_DOWN) {
            cursor_set(g, g->cursor + PAGE_SIZE);
            print_page(g, ctx);

        } else if (c == 'q' || c == 'Q') {
            gopher_print(ctx, "Gopher: closed");
            break;

        } else if (c == ESC || c == 'b' || c == 'B' || c == LEFT_ARROW) {
            uint8_t back_ip[4];
            uint16_t back_port;
            char back_sel[LINE_MAX];
            int back_cur = 0, back_scr = 0;
            if (!hist_pop(g, back_ip, &back_port, back_sel, &back_cur,
                          &back_scr)) {
                gopher_print(ctx, "Gopher: no history");
                if (c == ESC) {
                    gopher_print(ctx, "Gopher: closed");
                    break;
                }
                continue;
            }
            gopher_print(ctx, "Gopher: back");
            if (gopher_fetch(g, back_ip, back_port, back_sel, &fetch_len)) {
                memcpy(cur_ip, back_ip, 4);
                cur_port = back_port;
                gopher_strncpy(cur_sel, back_sel, LINE_MAX);
                parse_response(g, fetch_len);
                g->cursor = back_cur;
                g->scroll = back_scr;
                print_page(g, ctx);
            } else {
                gopher_print(ctx, "Gopher: fetch failed");
            }

        } else if (c == 'd' || c == 'D') {
            if (g->cursor < 0 || g->cursor >= g->line_count)
                continue;
            gopher_line_t *gl = &g->lines[g->cursor];
            if (!gl->is_file && !gl->is_link)
                continue;
            if (!resolve_gl(gl, cur_ip, ctx))
                continue;
            /* Use hostname if known, else cur_host (dotted IP or real name) */
            const char *dhost = gl->hostname[0] ? gl->hostname : cur_host;
            download_file(g, ctx, gl->host, gl->port, gl->selector, gl->type,
                          dhost);

        } else if (c == '\r' || c == '\n' || c == RIGHT_ARROW || c == ENTER) {
            if (g->cursor < 0 || g->cursor >= g->line_count)
                continue;
            gopher_line_t *gl = &g->lines[g->cursor];

            if (!gl->is_link && !gl->is_file)
                continue;

            if (!resolve_gl(gl, cur_ip, ctx))
                continue;

            if (gl->is_file && !gl->is_link) {
                const char *dhost = gl->hostname[0] ? gl->hostname : cur_host;
                download_file(g, ctx, gl->host, gl->port, gl->selector,
                              gl->type, dhost);
                continue;
            }

            char nav[TERM_BUF_LINE_W];
            int nm = 0;
            const char *pfx = "-> ";
            for (int j = 0; pfx[j]; j++)
                nav[nm++] = pfx[j];
            for (int j = 0; gl->display[j] && nm < TERM_BUF_LINE_W - 1; j++)
                nav[nm++] = gl->display[j];
            nav[nm] = 0;
            gopher_print(ctx, nav);

            uint8_t nav_ip[4];
            uint16_t nav_port = gl->port;
            char nav_sel[LINE_MAX];
            memcpy(nav_ip, gl->host, 4);
            gopher_strncpy(nav_sel, gl->selector, LINE_MAX);
            if (gl->hostname[0])
                gopher_strncpy(cur_host, gl->hostname, 64);

            if (gopher_fetch(g, nav_ip, nav_port, nav_sel, &fetch_len)) {
                hist_push(g, cur_ip, cur_port, cur_sel, g->cursor, g->scroll);
                memcpy(cur_ip, nav_ip, 4);
                cur_port = nav_port;
                gopher_strncpy(cur_sel, nav_sel, LINE_MAX);
                parse_response(g, fetch_len);
                /* Land on first selectable */
                g->cursor = 0;
                g->scroll = 0;
                {
                    int first = next_selectable(g, -1, 1);
                    if (first >= 0)
                        cursor_set(g, first);
                }
                print_page(g, ctx);
            } else {
                gopher_print(ctx, "Gopher: fetch failed");
            }
        }
    }
    if (ctx)
        asmterm_set_gopher_mode(ctx, false);
}

void gopher_run(term_context_t *ctx, const uint8_t start_ip[4], uint16_t port,
                const char *selector) {
    gopher_ctx_t *g = (gopher_ctx_t *)kmalloc(sizeof(gopher_ctx_t));
    if (!g) {
        gopher_print(ctx, "Gopher: out of memory");
        return;
    }
    memset(g, 0, sizeof(gopher_ctx_t));
    gopher_session(ctx, g, start_ip, port, selector);
    kfree(g);
}

void gopher_run_host(term_context_t *ctx, const char *hostname, uint16_t port,
                     const char *selector) {
    char msg[TERM_BUF_LINE_W];
    sprintf(msg, "Gopher: resolving %s", hostname);
    gopher_print(ctx, msg);

    uint8_t ip[4];
    if (!dns_resolve(hostname, ip)) {
        gopher_print(ctx, "Gopher: could not resolve hostname");
        sprintf(msg, "Gopher: resolved to %d.%d.%d.%d", ip[0], ip[1], ip[2],
                ip[3]);
        gopher_print(ctx, msg);
        return;
    }
    sprintf(msg, "Gopher: resolved to %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    gopher_print(ctx, msg);

    gopher_run(ctx, ip, port, selector);
}
