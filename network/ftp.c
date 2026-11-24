// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "network/ftp.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "network/net.h"
#include "os/scheduler.h"

#define REPLY_MS 15000 /* (a slow server, a big folder) */
#define DATA_MS 30000  /* no byte for this long: gone */

void ftp_init(ftp_t *c) {
    memset(c, 0, sizeof(*c));
    c->ctl = c->data = -1;
}

bool ftp_alive(const ftp_t *c) {
    return c->ctl >= 0 && (tcp_state(c->ctl) == TCP_ESTABLISHED);
}

static void drop(ftp_t *c) {
    if (c->data >= 0)
        tcp_release(c->data);
    if (c->ctl >= 0)
        tcp_release(c->ctl);
    c->data = c->ctl = -1;
    c->busy = false;
    c->ln = 0;
}

void ftp_close(ftp_t *c) {
    if (ftp_alive(c) && !c->busy) {
        tcp_send(c->ctl, (const uint8_t *)"QUIT\r\n", 6);
        tcp_close(c->ctl);
        c->ctl = -1;
    }
    drop(c);
}

static void wait_a_bit(void) {
    net_poll();
    task_yield();
}

/* the next line (without CR LF), NULL: none in time */
static const char *get_line(ftp_t *c, uint32_t ms) {
    static char line[sizeof(c->lb)];
    uint32_t until = time_millis() + ms;
    for (;;) {
        for (uint16_t i = 0; i < c->ln; i++) {
            if (c->lb[i] != '\n')
                continue;
            uint16_t n = i && c->lb[i - 1] == '\r' ? i - 1 : i;
            memcpy(line, c->lb, n);
            line[n] = 0;
            memmove(c->lb, c->lb + i + 1, c->ln - i - 1);
            c->ln -= i + 1;
            return line;
        }
        if (c->ln >= sizeof(c->lb) - 1)
            c->ln = 0; /* (a line too long: dropped) */
        int r = c->ctl < 0 ? -1
                           : tcp_recv(c->ctl, (uint8_t *)c->lb + c->ln,
                                      (uint16_t)(sizeof(c->lb) - 1 - c->ln));
        if (r > 0) {
            c->ln += (uint16_t)r;
            continue;
        }
        tcp_state_t st = tcp_state(c->ctl);
        if (r < 0 ||
            (st != TCP_ESTABLISHED && st != TCP_FIN_WAIT1 &&
             st != TCP_FIN_WAIT2) ||
            (int32_t)(time_millis() - until) >= 0)
            return NULL;
        wait_a_bit();
    }
}

static int code_of(const char *l) {
    if (l[0] < '1' || l[0] > '5' || l[1] < '0' || l[1] > '9' || l[2] < '0' ||
        l[2] > '9')
        return 0;
    return (l[0] - '0') * 100 + (l[1] - '0') * 10 + (l[2] - '0');
}

/* a whole reply ("123-" ... "123 "): its code, 0 = none */
static int reply(ftp_t *c, uint32_t ms, ftp_line_cb cb, void *user) {
    int code = 0;
    bool more = true;
    while (more) {
        const char *l = get_line(c, ms);
        if (!l) {
            drop(c);
            c->code = 0;
            snprintf(c->msg, sizeof(c->msg), "No answer from the server.");
            return 0;
        }
        int k = code_of(l);
        if (!code) {
            if (!k)
                continue; /* (noise before a reply) */
            code = k;
            more = l[3] == '-';
        } else if (k == code && l[3] != '-') {
            more = false;
        }
        const char *t = k && (l[3] == ' ' || l[3] == '-') ? l + 4
                        : k                               ? l + 3
                                                          : l;
        if (cb)
            cb(t, user);
        if (!more) {
            strncpy(c->msg, t, sizeof(c->msg) - 1);
            c->msg[sizeof(c->msg) - 1] = 0;
        }
    }
    c->code = code;
    return code;
}

static bool send_line(ftp_t *c, const char *cmd) {
    char b[600];
    int n = snprintf(b, sizeof(b), "%s\r\n", cmd);
    if (n >= (int)sizeof(b) ||
        !tcp_send(c->ctl, (const uint8_t *)b, (uint16_t)n)) {
        drop(c);
        c->code = 0;
        snprintf(c->msg, sizeof(c->msg), "The connection was lost.");
        return false;
    }
    return true;
}

int ftp_cmd_lines(ftp_t *c, const char *cmd, ftp_line_cb cb, void *user) {
    if (!ftp_alive(c) || c->busy) {
        c->code = 0;
        return 0;
    }
    if (!send_line(c, cmd))
        return 0;
    return reply(c, REPLY_MS, cb, user);
}

int ftp_cmd(ftp_t *c, const char *cmd) {
    return ftp_cmd_lines(c, cmd, NULL, NULL);
}

bool ftp_open(ftp_t *c, const uint8_t ip[4], uint16_t port, const char *user,
              const char *pass) {
    drop(c);
    memcpy(c->ip, ip, 4);
    c->port = port;
    c->code = 0;
    c->ctl = tcp_connect(ip, port, 0);
    if (c->ctl < 0) {
        snprintf(c->msg, sizeof(c->msg), "%s", "The server doesn't answer.");
        return false;
    }
    int k = reply(c, REPLY_MS, NULL, NULL);
    while (k >= 100 && k < 200) /* (120: ready in a moment) */
        k = reply(c, REPLY_MS, NULL, NULL);
    if (k != 220) {
        drop(c);
        return false;
    }
    char b[160];
    snprintf(b, sizeof(b), "USER %s", user && *user ? user : "anonymous");
    k = ftp_cmd(c, b);
    if (k == 331 || k == 332) {
        snprintf(b, sizeof(b), "PASS %s", pass && *pass ? pass : "asmos@");
        k = ftp_cmd(c, b);
    }
    if (k != 230 && k != 202) {
        if (k)
            ftp_close(c);
        return false;
    }
    if (ftp_cmd(c, "TYPE I") != 200) {
        ftp_close(c);
        return false;
    }
    return true;
}

/* "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)": the port (the address
 * is the server's own: one behind a router may name its inside one) */
static uint16_t pasv_port(const char *m) {
    const char *p = m;
    while (*p && (*p < '0' || *p > '9'))
        p++;
    int v[6], n = 0;
    while (n < 6 && *p) {
        if (*p < '0' || *p > '9')
            break;
        int x = 0;
        while (*p >= '0' && *p <= '9')
            x = x * 10 + (*p++ - '0');
        v[n++] = x;
        if (*p == ',')
            p++;
    }
    if (n < 6 || v[4] > 255 || v[5] > 255)
        return 0;
    return (uint16_t)(v[4] << 8 | v[5]);
}

/* "229 Entering Extended Passive Mode (|||6446|)" */
static uint16_t epsv_port(const char *m) {
    const char *p = strstr(m, "(|||");
    if (!p)
        return 0;
    p += 4;
    uint32_t x = 0;
    while (*p >= '0' && *p <= '9')
        x = x * 10 + (uint32_t)(*p++ - '0');
    return *p == '|' && x && x < 65536 ? (uint16_t)x : 0;
}

bool ftp_xfer_begin(ftp_t *c, const char *cmd, uint32_t rest) {
    if (!ftp_alive(c) || c->busy)
        return false;
    uint16_t port = 0;
    int k = ftp_cmd(c, "PASV");
    if (k == 227)
        port = pasv_port(c->msg);
    else if (k && ftp_cmd(c, "EPSV") == 229)
        port = epsv_port(c->msg);
    if (!port)
        return false;
    c->data = tcp_connect(c->ip, port, 0);
    if (c->data < 0) {
        snprintf(c->msg, sizeof(c->msg), "No data connection.");
        return false;
    }
    if (rest) {
        char b[24];
        snprintf(b, sizeof(b), "REST %u", rest);
        if (ftp_cmd(c, b) != 350) {
            tcp_release(c->data);
            c->data = -1;
            return false;
        }
    }
    if (!send_line(c, cmd))
        return false;
    k = reply(c, REPLY_MS, NULL, NULL);
    if (k == 125 || k == 150) {
        c->busy = true;
        return true;
    }
    tcp_release(c->data); /* (550: no such file, ...) */
    c->data = -1;
    return false;
}

int ftp_xfer_read(ftp_t *c, uint8_t *buf, uint16_t n) {
    if (c->data < 0)
        return 0;
    uint32_t last = time_millis();
    for (;;) {
        int r = tcp_recv(c->data, buf, n);
        if (r)
            return r;
        tcp_state_t st = tcp_state(c->data);
        if (st != TCP_ESTABLISHED)
            return tcp_was_reset(c->data) ? -1 : 0; /* (all of it came) */
        if (time_millis() - last > DATA_MS)
            return -1;
        wait_a_bit();
    }
}

bool ftp_xfer_write(ftp_t *c, const uint8_t *buf, uint16_t n) {
    return c->data >= 0 && tcp_send(c->data, buf, n);
}

bool ftp_xfer_end(ftp_t *c, bool abort) {
    if (!c->busy)
        return false;
    if (c->data >= 0) {
        if (abort)
            tcp_abort(c->data); /* (the server sees it cut short) */
        else
            tcp_close(c->data); /* (an upload: all of it acknowledged) */
        c->data = -1;
    }
    c->busy = false;
    int k = reply(c, abort ? 3000 : DATA_MS, NULL, NULL);
    if (abort && k >= 400 && k < 500) { /* 426, then maybe a 226 */
        uint32_t until = time_millis() + 300;
        while (c->ln == 0 && time_millis() < until)
            wait_a_bit();
        if (c->ln)
            reply(c, 1000, NULL, NULL);
        return true;
    }
    return k >= 200 && k < 300;
}
