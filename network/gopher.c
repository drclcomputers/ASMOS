#include "network/gopher.h"
#include "drivers/ne2000.h"
#include "io/keyboard.h"
#include "lib/core.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/time.h"
#include "network/dns.h"
#include "network/net.h"
#include "os/scheduler.h"
#include "shell/term_buf.h"

#define GOPHER_PORT 70
#define GOPHER_LOCAL 49152
#define LINE_MAX 255
#define MAX_LINES 128
#define VISIBLE_LINES 18
#define SCREEN_Y 30
#define LINE_H 12
#define FETCH_BUF_SIZE 8192
#define HIST_MAX 16

typedef struct {
    char type;
    char display[LINE_MAX];
    char selector[LINE_MAX];
    uint8_t host[4];
    uint16_t port;
    bool is_link;
} gopher_line_t;

typedef struct {
    uint8_t ip[4];
    uint16_t port;
    char sel[LINE_MAX];
} hist_entry_t;

static gopher_line_t *s_lines = NULL;
static char *s_fetch_buf = NULL;
static hist_entry_t *s_history = NULL;

static int s_line_count = 0;
static int s_cursor = 0;
static int s_scroll = 0;
static int s_hist_top = 0;

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

static int gopher_strncpy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i;
}

static void parse_response(const char *buf, int len) {
    s_line_count = 0;
    int i = 0;
    while (i < len && s_line_count < MAX_LINES) {
        if (buf[i] == '.') {
            i++;
            break;
        }

        char type = buf[i++];
        gopher_line_t *gl = &s_lines[s_line_count];
        gl->type = type;
        gl->is_link = false;
        memset(gl->display, 0, LINE_MAX);
        memset(gl->selector, 0, LINE_MAX);
        memset(gl->host, 0, 4);
        gl->port = 70;

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
                    if (!parse_ip_str(hostbuf, gl->host))
                        dns_resolve(hostbuf, gl->host);
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
        s_line_count++;
    }
}

static bool gopher_fetch(const uint8_t ip[4], uint16_t port,
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
        int n = tcp_recv(sock, (uint8_t *)s_fetch_buf + total,
                         FETCH_BUF_SIZE - total);
        if (n > 0) {
            total += n;
            deadline = time_millis() + 1000;
        }
        if (total >= FETCH_BUF_SIZE)
            break;
        if (tcp_state(sock) == TCP_CLOSE_WAIT || tcp_state(sock) == TCP_CLOSED)
            break;
    }
    tcp_close(sock);
    *out_len = total;
    return total > 0;
}

static void render_gopher(void) {
    clear_screen(BLACK);
    term_buf_push("GOPHER");
    term_buf_push("- UP/DOWN: navigate  ENTER: open  ESC: back");

    for (int i = 0; i < VISIBLE_LINES && (s_scroll + i) < s_line_count; i++) {
        int li = s_scroll + i;
        gopher_line_t *gl = &s_lines[li];
        int y = SCREEN_Y + i * LINE_H;
        uint8_t fg = WHITE;
        char prefix[3] = "  ";

        if (gl->type == '1') {
            fg = CYAN;
            prefix[0] = '[';
            prefix[1] = '>';
        } else if (gl->type == '0') {
            fg = LIGHT_GREEN;
            prefix[0] = '[';
            prefix[1] = 'T';
        } else if (gl->type == 'i') {
            fg = LIGHT_GRAY;
            prefix[0] = ' ';
            prefix[1] = ' ';
        } else {
            fg = DARK_GRAY;
        }
        (void)fg;

        if (li == s_cursor) {
            for (int bx = 0; bx < g_screen_width; bx++)
                draw_dot(bx, y, BLUE);
        }
        term_buf_push(prefix);
        term_buf_push(gl->display);
    }

    char status[32];
    int si = 0;
    const char *lbl = "Line ";
    for (int j = 0; lbl[j]; j++)
        status[si++] = lbl[j];
    int num = s_cursor + 1;
    if (num >= 100)
        status[si++] = '0' + num / 100;
    if (num >= 10)
        status[si++] = '0' + (num / 10) % 10;
    status[si++] = '0' + num % 10;
    status[si++] = '/';
    num = s_line_count;
    if (num >= 100)
        status[si++] = '0' + num / 100;
    if (num >= 10)
        status[si++] = '0' + (num / 10) % 10;
    status[si++] = '0' + num % 10;
    status[si] = 0;
    term_buf_push(status);
    blit();
}

static void gopher_dump_to_termbuf(void) {
    char line[TERM_BUF_LINE_W];
    for (int i = 0; i < s_line_count; i++) {
        gopher_line_t *gl = &s_lines[i];
        const char *prefix = (gl->type == '1')   ? "[>]"
                             : (gl->type == '0') ? "[T]"
                                                 : "   ";
        int p = 0;
        for (int j = 0; prefix[j] && p < TERM_BUF_LINE_W - 2; j++)
            line[p++] = prefix[j];
        line[p++] = ' ';
        for (int j = 0; gl->display[j] && p < TERM_BUF_LINE_W - 1; j++)
            line[p++] = gl->display[j];
        line[p] = 0;
        term_buf_push(line);
    }
}

// Helpers
static void hist_push(const uint8_t ip[4], uint16_t port, const char *sel) {
    if (s_hist_top < HIST_MAX) {
        memcpy(s_history[s_hist_top].ip, ip, 4);
        s_history[s_hist_top].port = port;
        gopher_strncpy(s_history[s_hist_top].sel, sel, LINE_MAX);
        s_hist_top++;
    }
}

static bool hist_pop(uint8_t ip[4], uint16_t *port, char *sel) {
    if (s_hist_top <= 1)
        return false;
    s_hist_top--;
    memcpy(ip, s_history[s_hist_top - 1].ip, 4);
    *port = s_history[s_hist_top - 1].port;
    gopher_strncpy(sel, s_history[s_hist_top - 1].sel, LINE_MAX);
    return true;
}

static bool gopher_alloc(void) {
    s_lines = (gopher_line_t *)kmalloc(MAX_LINES * sizeof(gopher_line_t));
    s_fetch_buf = (char *)kmalloc(FETCH_BUF_SIZE);
    s_history = (hist_entry_t *)kmalloc(HIST_MAX * sizeof(hist_entry_t));
    return s_lines && s_fetch_buf && s_history;
}

static void gopher_free(void) {
    kfree(s_lines);
    s_lines = NULL;
    kfree(s_fetch_buf);
    s_fetch_buf = NULL;
    kfree(s_history);
    s_history = NULL;
}

// Public API
void gopher_run(const uint8_t start_ip[4], uint16_t port,
                const char *selector) {
    if (!gopher_alloc()) {
        term_buf_push("Gopher: out of memory");
        gopher_free();
        return;
    }

    int fetch_len = 0;
    uint8_t cur_ip[4];
    uint16_t cur_port = port;
    char cur_sel[LINE_MAX];
    memcpy(cur_ip, start_ip, 4);
    gopher_strncpy(cur_sel, selector, LINE_MAX);

    s_hist_top = 0;
    hist_push(cur_ip, cur_port, cur_sel);

    term_buf_push("--- Gopher: connecting ---");
    blit();

    if (!gopher_fetch(cur_ip, cur_port, cur_sel, &fetch_len)) {
        term_buf_push("Gopher: connection failed");
        sleep_ms(2000);
        gopher_free();
        return;
    }

    parse_response(s_fetch_buf, fetch_len);
    gopher_dump_to_termbuf();
    s_cursor = 0;
    s_scroll = 0;
    render_gopher();

    while (1) {
        kb_update();
        net_poll();
        task_yield();
        if (inb(0x64) & 0x01)
            kb_process_byte(inb(0x60));
        if (!kb.key_pressed)
            continue;

        uint8_t key = kb.last_scancode;

        if (key == UP_ARROW && s_cursor > 0) {
            s_cursor--;
            if (s_cursor < s_scroll)
                s_scroll = s_cursor;
            render_gopher();

        } else if (key == DOWN_ARROW && s_cursor < s_line_count - 1) {
            s_cursor++;
            if (s_cursor >= s_scroll + VISIBLE_LINES)
                s_scroll = s_cursor - VISIBLE_LINES + 1;
            render_gopher();

        } else if (key == ENTER) {
            gopher_line_t *gl = &s_lines[s_cursor];
            if (!gl->is_link)
                continue;

            char nav_msg[TERM_BUF_LINE_W];
            int nm = 0;
            const char *pfx = "-> ";
            for (int j = 0; pfx[j]; j++)
                nav_msg[nm++] = pfx[j];
            for (int j = 0; gl->display[j] && nm < TERM_BUF_LINE_W - 1; j++)
                nav_msg[nm++] = gl->display[j];
            nav_msg[nm] = 0;
            term_buf_push(nav_msg);

            hist_push(gl->host, gl->port, gl->selector);
            blit();
            if (gopher_fetch(gl->host, gl->port, gl->selector, &fetch_len)) {
                parse_response(s_fetch_buf, fetch_len);
                gopher_dump_to_termbuf();
                s_cursor = 0;
                s_scroll = 0;
                render_gopher();
            } else {
                term_buf_push("Gopher: fetch failed");
                blit();
                sleep_ms(1500);
                render_gopher();
            }

        } else if (key == ESC) {
            uint8_t back_ip[4];
            uint16_t back_port;
            char back_sel[LINE_MAX];
            if (hist_pop(back_ip, &back_port, back_sel)) {
                term_buf_push("<- back");
                blit();
                if (gopher_fetch(back_ip, back_port, back_sel, &fetch_len)) {
                    parse_response(s_fetch_buf, fetch_len);
                    gopher_dump_to_termbuf();
                    s_cursor = 0;
                    s_scroll = 0;
                    render_gopher();
                }
            } else {
                term_buf_push("--- Gopher: closed ---");
                break;
            }
        }
    }

    gopher_free();
}

void gopher_run_host(const char *hostname, uint16_t port,
                     const char *selector) {
    uint8_t ip[4];
    char msg[TERM_BUF_LINE_W];
    int ml = 0;
    const char *rpfx = "Resolving: ";
    for (int i = 0; rpfx[i]; i++)
        msg[ml++] = rpfx[i];
    for (int i = 0; hostname[i] && ml < TERM_BUF_LINE_W - 1; i++)
        msg[ml++] = hostname[i];
    msg[ml] = 0;
    term_buf_push(msg);

    if (dns_resolve(hostname, ip)) {
        gopher_run(ip, port, selector);
    } else {
        term_buf_push("Gopher: could not resolve hostname");
        blit();
        sleep_ms(2000);
    }
}
