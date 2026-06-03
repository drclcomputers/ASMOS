#include "network/gopher.h"
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
#define MAX_LINES 128
#define FETCH_BUF_SIZE 8192
#define HIST_MAX 16

typedef struct {
    char type;
    char display[LINE_MAX];
    char selector[LINE_MAX];
    uint8_t host[4];
    uint16_t port;
    bool is_link;
    char hostname[64];
} gopher_line_t;

typedef struct {
    uint8_t ip[4];
    uint16_t port;
    char sel[LINE_MAX];
} hist_entry_t;

typedef struct {
    gopher_line_t lines[MAX_LINES];
    char fetch_buf[FETCH_BUF_SIZE];
    hist_entry_t history[HIST_MAX];
    int line_count;
    int cursor;
    int hist_top;
} gopher_ctx_t;

/* ── helpers ──────────────────────────────────────────────────────────── */
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

/* ── parser ───────────────────────────────────────────────────────────── */
static void parse_response(gopher_ctx_t *g, int len) {
    const char *buf = g->fetch_buf;
    g->line_count = 0;
    int i = 0;
    while (i < len && g->line_count < MAX_LINES) {
        if (buf[i] == '.') {
            i++;
            break;
        }

        char type = buf[i++];
        gopher_line_t *gl = &g->lines[g->line_count];
        gl->type = type;
        gl->is_link = false;
        memset(gl->display, 0, LINE_MAX);
        memset(gl->selector, 0, LINE_MAX);
        memset(gl->host, 0, 4);
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

        if (type == '0' || type == '1')
            gl->is_link = true;
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

/* ── display ──────────────────────────────────────────────────────────── */
static void print_page(gopher_ctx_t *g, term_context_t *ctx) {
    char line[TERM_BUF_LINE_W];

    gopher_print(ctx,
                 "--- Gopher ---  UP/DOWN:j/k  ENTER:open  b:back  q:quit");

    for (int i = 0; i < g->line_count; i++) {
        gopher_line_t *gl = &g->lines[i];

        const char *prefix;
        if (gl->type == '1')
            prefix = "[>]";
        else if (gl->type == '0')
            prefix = "[T]";
        else
            prefix = "   ";

        const char *cursor = (i == g->cursor) ? ">" : " ";

        int p = 0;
        line[p++] = cursor[0];
        line[p++] = ' ';
        for (int j = 0; prefix[j] && p < TERM_BUF_LINE_W - 2; j++)
            line[p++] = prefix[j];
        line[p++] = ' ';
        for (int j = 0; gl->display[j] && p < TERM_BUF_LINE_W - 1; j++)
            line[p++] = gl->display[j];
        line[p] = 0;
        gopher_print(ctx, line);
    }

    char status[32];
    sprintf(status, "  Line %d/%d", g->cursor + 1, g->line_count);
    gopher_print(ctx, status);
    gopher_print(ctx, "");
    gopher_print(ctx, "");
}

/* ── history ──────────────────────────────────────────────────────────── */
static void hist_push(gopher_ctx_t *g, const uint8_t ip[4], uint16_t port,
                      const char *sel) {
    if (g->hist_top < HIST_MAX) {
        memcpy(g->history[g->hist_top].ip, ip, 4);
        g->history[g->hist_top].port = port;
        gopher_strncpy(g->history[g->hist_top].sel, sel, LINE_MAX);
        g->hist_top++;
    }
}

static bool hist_pop(gopher_ctx_t *g, uint8_t ip[4], uint16_t *port,
                     char *sel) {
    if (g->hist_top <= 1)
        return false;
    g->hist_top--;
    memcpy(ip, g->history[g->hist_top - 1].ip, 4);
    *port = g->history[g->hist_top - 1].port;
    gopher_strncpy(sel, g->history[g->hist_top - 1].sel, LINE_MAX);
    return true;
}

/* ── core session loop ────────────────────────────────────────────────── */
static void gopher_session(term_context_t *ctx, gopher_ctx_t *g,
                           const uint8_t start_ip[4], uint16_t port,
                           const char *selector) {
    if (ctx)
        asmterm_set_gopher_mode(ctx, true);
    int fetch_len = 0;
    uint8_t cur_ip[4];
    uint16_t cur_port = port;
    char cur_sel[LINE_MAX];
    memcpy(cur_ip, start_ip, 4);
    gopher_strncpy(cur_sel, selector, LINE_MAX);

    g->hist_top = 0;
    hist_push(g, cur_ip, cur_port, cur_sel);

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

    parse_response(g, fetch_len);

    {
        char dbg[48];
        sprintf(dbg, "Gopher: parsed %d lines", g->line_count);
        gopher_print(ctx, dbg);
    }
    g->cursor = 0;
    print_page(g, ctx);

    while (1) {
        int c = ctx->getchar(ctx);

        if (c == 'q' || c == 'Q') {
            gopher_print(ctx, "Gopher: closed");
            break;
        } else if (c == ESC || c == 'b' || c == 'B') {
            uint8_t back_ip[4];
            uint16_t back_port;
            char back_sel[LINE_MAX];
            if (!hist_pop(g, back_ip, &back_port, back_sel)) {
                gopher_print(ctx, "Gopher: no history");
                if (c == 27) {
                    gopher_print(ctx, "Gopher: closed");
                    break;
                }
                continue;
            }
            gopher_print(ctx, "Gopher: back");
            if (gopher_fetch(g, back_ip, back_port, back_sel, &fetch_len)) {
                parse_response(g, fetch_len);
                g->cursor = 0;
                print_page(g, ctx);
            } else {
                gopher_print(ctx, "Gopher: fetch failed");
            }

        } else if (c == 'k' || c == 'K') {
            if (g->cursor > 0) {
                g->cursor--;
                print_page(g, ctx);
            }

        } else if (c == 'j' || c == 'J') {
            if (g->cursor < g->line_count - 1) {
                g->cursor++;
                print_page(g, ctx);
            }

        } else if (c == '\r' || c == '\n') {
            if (g->cursor < 0 || g->cursor >= g->line_count)
                continue;
            gopher_line_t *gl = &g->lines[g->cursor];
            if (!gl->is_link)
                continue;

            uint8_t zero[4] = {0, 0, 0, 0};
            if (memcmp(gl->host, zero, 4) == 0 && gl->hostname[0]) {
                gopher_print(ctx, "Gopher: resolving...");
                if (!dns_resolve(gl->hostname, gl->host)) {
                    gopher_print(ctx, "Gopher: DNS failed");
                    continue;
                }
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

            hist_push(g, gl->host, gl->port, gl->selector);
            if (gopher_fetch(g, gl->host, gl->port, gl->selector, &fetch_len)) {
                parse_response(g, fetch_len);
                g->cursor = 0;
                print_page(g, ctx);
            } else {
                gopher_print(ctx, "Gopher: fetch failed");
            }
        }
    }
    if (ctx)
        asmterm_set_gopher_mode(ctx, false);
}

/* ── public API ───────────────────────────────────────────────────────── */
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
        return;
    }
    gopher_run(ctx, ip, port, selector);
}
