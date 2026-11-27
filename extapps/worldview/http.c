// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* URLs, and HTTP/1.0 without waiting: fetch_step moves it on a step each
 * frame (look the name up, connect, send, read), so the window stays
 * alive. Redirects are followed (5 at most), chunked answers put back
 * together. Through a proxy ("host:port") the whole URL is asked for,
 * https:// too: the proxy does the encryption. */

#include "extapps/worldview/wv.h"

wv_net_cfg_t g_wv_net;

#define CONNECT_MS 15000
#define IDLE_MS 30000

// URLs

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

bool url_split(const char *url, wv_url_t *u) {
    memset(u, 0, sizeof(*u));
    const char *p = url;
    if (!strncasecmp(p, "http://", 7)) {
        p += 7;
        u->port = 80;
    } else if (!strncasecmp(p, "https://", 8)) {
        p += 8;
        u->https = true;
        u->port = 443;
    } else {
        return false;
    }
    int n = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (n < (int)sizeof(u->host) - 1)
            u->host[n++] = (char)lower(*p);
        p++;
    }
    u->host[n] = 0;
    if (!n)
        return false;
    if (*p == ':') {
        uint32_t port = 0;
        for (p++; *p >= '0' && *p <= '9'; p++)
            port = port * 10 + (uint32_t)(*p - '0');
        if (port == 0 || port > 65535)
            return false;
        u->port = (uint16_t)port;
    }
    if (*p != '/')
        u->path[0] = '/';
    n = *p == '/' ? 0 : 1;
    for (; *p && *p != '#' && n < WV_URL - 1; p++)
        u->path[n++] = *p == ' ' ? '+' : *p;
    u->path[n] = 0;
    return true;
}

/* "a/b/../c" -> "a/c", "./" dropped */
static void tidy_path(char *p) {
    char *q = strchr(p, '?');
    char keep[WV_URL];
    keep[0] = 0;
    if (q) {
        strlcpy(keep, q, sizeof(keep));
        *q = 0;
    }
    char out[WV_URL];
    int o = 0;
    const char *s = p;
    while (*s) {
        if (s[0] == '/' && s[1] == '.' && (s[2] == '/' || !s[2])) {
            s += 2;
            if (!*s)
                out[o++] = '/';
            continue;
        }
        if (s[0] == '/' && s[1] == '.' && s[2] == '.' &&
            (s[3] == '/' || !s[3])) {
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
            while (o > 0 && out[o - 1] != '/')
                o--;
            s += 3;
            if (!*s || *s == '/') {
                if (o == 0 || out[o - 1] != '/')
                    out[o++] = '/';
                if (*s == '/')
                    s++;
            }
            continue;
        }
        if (o < WV_URL - 1)
            out[o++] = *s;
        s++;
    }
    out[o] = 0;
    if (!o)
        strcpy(out, "/");
    snprintf(p, WV_URL, "%s%s", out, keep);
}

void url_join(const char *base, const char *rel, char *out, int sz) {
    while (*rel == ' ' || *rel == '\n' || *rel == '\r' || *rel == '\t')
        rel++;
    if (!strncasecmp(rel, "http://", 7) || !strncasecmp(rel, "https://", 8)) {
        strlcpy(out, rel, (size_t)sz);
        return;
    }
    wv_url_t b;
    if (!url_split(base, &b)) {
        strlcpy(out, rel, (size_t)sz);
        return;
    }
    char origin[128];
    bool std_port = b.port == (b.https ? 443 : 80);
    snprintf(origin, sizeof(origin), "%s://%s", b.https ? "https" : "http",
             b.host);
    if (!std_port) {
        char pt[8];
        snprintf(pt, sizeof(pt), ":%u", b.port);
        strlcat(origin, pt, sizeof(origin));
    }
    char path[WV_URL];
    if (rel[0] == '/' && rel[1] == '/') { /* //host/path */
        snprintf(out, sz, "%s:%s", b.https ? "https" : "http", rel);
        return;
    }
    if (rel[0] == '/') {
        strlcpy(path, rel, sizeof(path));
    } else if (rel[0] == '?') {
        strlcpy(path, b.path, sizeof(path));
        char *q = strchr(path, '?');
        if (q)
            *q = 0;
        strlcat(path, rel, sizeof(path));
    } else if (!rel[0] || rel[0] == '#') {
        strlcpy(path, b.path, sizeof(path));
    } else {
        strlcpy(path, b.path, sizeof(path));
        char *q = strchr(path, '?');
        if (q)
            *q = 0;
        char *slash = strrchr(path, '/');
        if (slash)
            slash[1] = 0;
        strlcat(path, rel, sizeof(path));
    }
    char *hash = strchr(path, '#');
    if (hash)
        *hash = 0;
    tidy_path(path);
    snprintf(out, sz, "%s%s", origin, path);
}

int url_encode(const char *in, char *out, int sz) {
    static const char HEX[] = "0123456789ABCDEF";
    int o = 0;
    for (; *in && o < sz - 1; in++) {
        uint8_t c = (uint8_t)*in;
        char u[4];
        int n = 1;
        u[0] = (char)c;
        if (c >= 0x80)
            n = txt_cp_to_utf8(c, u);
        for (int k = 0; k < n && o < sz - 1; k++) {
            uint8_t b = (uint8_t)u[k];
            if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
                (b >= '0' && b <= '9') || b == '-' || b == '_' || b == '.' ||
                b == '*') {
                out[o++] = (char)b;
            } else if (b == ' ') {
                out[o++] = '+';
            } else if (o + 3 < sz) {
                out[o++] = '%';
                out[o++] = HEX[b >> 4];
                out[o++] = HEX[b & 15];
            } else {
                break;
            }
        }
    }
    out[o] = 0;
    return o;
}

// fetching

static void fail(wv_fetch_t *f, const char *why) {
    if (f->sock >= 0)
        tcp_abort(f->sock);
    if (f->dns >= 0)
        dns_cancel(f->dns);
    f->sock = f->dns = -1;
    strlcpy(f->err, why, sizeof(f->err));
    f->state = F_FAILED;
}

/* where to connect: the proxy, or the URL's host */
static bool target(const wv_fetch_t *f, wv_url_t *u, char *host, int hsz,
                   uint16_t *port) {
    if (!url_split(f->url, u))
        return false;
    if (g_wv_net.proxy[0]) {
        strlcpy(host, g_wv_net.proxy, (size_t)hsz);
        char *colon = strchr(host, ':');
        *port = 8080;
        if (colon) {
            *colon = 0;
            *port = (uint16_t)str_to_uint32(colon + 1);
        }
        return host[0] != 0;
    }
    strlcpy(host, u->host, (size_t)hsz);
    *port = u->port;
    return true;
}

static void begin(wv_fetch_t *f) {
    wv_url_t u;
    char host[80];
    uint16_t port;
    f->head_len = 0;
    f->head_done = false;
    f->status = 0;
    f->ctype[0] = f->charset[0] = f->location[0] = 0;
    f->chunked = false;
    f->clen = -1;
    f->len = 0;
    f->truncated = false;
    f->chunk_st = 0;
    f->chunk_left = 0;
    f->chunk_line_len = 0;
    if (!target(f, &u, host, sizeof(host), &port)) {
        fail(f, "That is not a web address (http://...).");
        return;
    }
    if (u.https && !g_wv_net.proxy[0]) {
        fail(f, "Secure sites (https://) need a proxy: Options > Proxy.");
        return;
    }
    if (!net_ready()) {
        fail(f, "No network: see Settings > Network.");
        return;
    }
    kfree(f->req);
    f->req = (char *)kmalloc(WV_URL + 512 + (uint32_t)f->post_len);
    if (!f->req) {
        fail(f, "Not enough memory.");
        return;
    }
    const char *what = g_wv_net.proxy[0] ? f->url : u.path;
    char hosthdr[96];
    if (u.port == (u.https ? 443 : 80))
        strlcpy(hosthdr, u.host, sizeof(hosthdr));
    else
        snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u.host, u.port);
    f->req_len = snprintf(
        f->req, WV_URL + 512,
        "%s %s HTTP/1.0\r\nHost: %s\r\n"
        "User-Agent: WorldView/1.0 (ASMOS; i386)\r\n"
        "Accept: text/html, text/plain, image/gif, image/jpeg, image/bmp, "
        "*/*;q=0.5\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
        f->post ? "POST" : "GET", what, hosthdr);
    if (f->post) {
        f->req_len += snprintf(f->req + f->req_len, 200,
                               "Content-Type: application/x-www-form-"
                               "urlencoded\r\nContent-Length: %d\r\n",
                               f->post_len);
    }
    f->req_len += snprintf(f->req + f->req_len, 3, "\r\n");
    if (f->post) {
        memcpy(f->req + f->req_len, f->post, (uint32_t)f->post_len);
        f->req_len += f->post_len;
    }
    f->req_sent = 0;
    f->dns = dns_start(host);
    f->port = port;
    f->state = f->dns >= 0 ? F_DNS : F_FAILED;
    if (f->dns < 0)
        strcpy(f->err, "Could not look the name up.");
    f->since = time_millis();
}

void fetch_start(wv_fetch_t *f, const char *url, const char *post, int post_len,
                 uint32_t max) {
    fetch_free(f);
    memset(f, 0, sizeof(*f));
    f->sock = f->dns = -1;
    f->max = max;
    strlcpy(f->url, url, sizeof(f->url));
    if (post) {
        f->post = (char *)kmalloc((uint32_t)post_len + 1);
        if (f->post) {
            memcpy(f->post, post, (uint32_t)post_len);
            f->post_len = post_len;
        }
    }
    begin(f);
}

void fetch_free(wv_fetch_t *f) {
    if (f->sock >= 0)
        tcp_abort(f->sock);
    if (f->dns >= 0)
        dns_cancel(f->dns);
    f->sock = f->dns = -1;
    kfree(f->req);
    kfree(f->post);
    kfree(f->body);
    f->req = f->post = NULL;
    f->body = NULL;
    f->len = f->cap = 0;
    if (f->state != F_DONE && f->state != F_FAILED)
        f->state = F_IDLE;
}

/* n more bytes of the body */
static void keep(wv_fetch_t *f, const uint8_t *b, uint32_t n) {
    if (f->len + n > f->max) {
        n = f->max > f->len ? f->max - f->len : 0;
        f->truncated = true;
    }
    if (f->len + n > f->cap) {
        uint32_t cap = f->cap ? f->cap : 8192;
        while (cap < f->len + n)
            cap *= 2;
        if (cap > f->max)
            cap = f->max;
        uint8_t *nb = (uint8_t *)kmalloc(cap + 1);
        if (!nb) {
            f->truncated = true;
            return;
        }
        if (f->body)
            memcpy(nb, f->body, f->len);
        kfree(f->body);
        f->body = nb;
        f->cap = cap;
    }
    memcpy(f->body + f->len, b, n);
    f->len += n;
    f->body[f->len] = 0;
}

/* the body, through the chunks if it comes in chunks */
static void body_bytes(wv_fetch_t *f, const uint8_t *b, uint32_t n) {
    if (!f->chunked) {
        keep(f, b, n);
        return;
    }
    uint32_t i = 0;
    while (i < n && f->chunk_st != 3) {
        if (f->chunk_st == 0 || f->chunk_st == 2) { /* a size line, or the
                                                     * CRLF after data */
            char c = (char)b[i++];
            if (c == '\n') {
                f->chunk_line[f->chunk_line_len] = 0;
                if (f->chunk_st == 2) {
                    f->chunk_st = 0;
                } else {
                    f->chunk_left = (int)str_to_ulong(f->chunk_line, NULL, 16);
                    f->chunk_st = f->chunk_left ? 1 : 3;
                }
                f->chunk_line_len = 0;
            } else if (c != '\r' &&
                       f->chunk_line_len < (int)sizeof(f->chunk_line) - 1) {
                f->chunk_line[f->chunk_line_len++] = c;
            }
        } else {
            uint32_t take = n - i < (uint32_t)f->chunk_left
                                ? n - i
                                : (uint32_t)f->chunk_left;
            keep(f, b + i, take);
            i += take;
            f->chunk_left -= (int)take;
            if (!f->chunk_left)
                f->chunk_st = 2;
        }
    }
}

static void header_value(const char *line, const char *name, char *out,
                         int sz) {
    int n = (int)strlen(name);
    if (strncasecmp(line, name, (size_t)n) || line[n] != ':')
        return;
    line += n + 1;
    while (*line == ' ' || *line == '\t')
        line++;
    strlcpy(out, line, (size_t)sz);
}

/* the head read: what it says */
static void parse_head(wv_fetch_t *f) {
    char *p = f->head;
    char *nl = strchr(p, '\n');
    if (!strncmp(p, "HTTP/", 5)) {
        char *sp = strchr(p, ' ');
        f->status = sp ? (int)str_to_uint32(sp + 1) : 0;
    }
    char v[WV_URL];
    while (nl) {
        char *line = nl + 1;
        nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        int ln = (int)strlen(line);
        if (ln && line[ln - 1] == '\r')
            line[ln - 1] = 0;
        v[0] = 0;
        header_value(line, "Content-Type", v, sizeof(v));
        if (v[0]) {
            int i = 0;
            for (; v[i] && v[i] != ';' && i < (int)sizeof(f->ctype) - 1; i++)
                f->ctype[i] = (char)lower(v[i]);
            f->ctype[i] = 0;
            char *cs = strstr(v, "charset=");
            if (cs)
                for (i = 0; cs[8 + i] && cs[8 + i] != ';' &&
                            i < (int)sizeof(f->charset) - 1;
                     i++)
                    f->charset[i] = (char)lower(cs[8 + i]);
            continue;
        }
        header_value(line, "Location", f->location, sizeof(f->location));
        v[0] = 0;
        header_value(line, "Content-Length", v, sizeof(v));
        if (v[0])
            f->clen = (int32_t)str_to_uint32(v);
        v[0] = 0;
        header_value(line, "Transfer-Encoding", v, sizeof(v));
        if (v[0] && strstr(v, "chunked"))
            f->chunked = true;
    }
}

/* bytes from the connection: the head first, then the body */
static void got(wv_fetch_t *f, const uint8_t *b, uint32_t n) {
    uint32_t i = 0;
    while (!f->head_done && i < n) {
        if (f->head_len < (int)sizeof(f->head) - 1)
            f->head[f->head_len++] = (char)b[i];
        i++;
        f->head[f->head_len] = 0;
        int L = f->head_len;
        if ((L >= 4 && !memcmp(f->head + L - 4, "\r\n\r\n", 4)) ||
            (L >= 2 && !memcmp(f->head + L - 2, "\n\n", 2))) {
            f->head_done = true;
            parse_head(f);
        }
    }
    if (i < n)
        body_bytes(f, b + i, n - i);
}

static bool finished(const wv_fetch_t *f) {
    if (!f->head_done)
        return false;
    if (f->chunked)
        return f->chunk_st == 3;
    return f->clen >= 0 && f->len >= (uint32_t)f->clen;
}

/* done: a redirect starts again, else it is DONE */
static void complete(wv_fetch_t *f) {
    if (f->sock >= 0)
        tcp_release(f->sock);
    f->sock = -1;
    if (!f->head_done) {
        fail(f, "The server sent nothing back.");
        return;
    }
    bool redirect = f->status == 301 || f->status == 302 || f->status == 303 ||
                    f->status == 307 || f->status == 308;
    if (redirect && f->location[0]) {
        if (++f->redirects > 5) {
            fail(f, "Too many redirects.");
            return;
        }
        char next[WV_URL];
        url_join(f->url, f->location, next, sizeof(next));
        strlcpy(f->url, next, sizeof(f->url));
        if (f->status != 307 && f->status != 308) { /* (then a GET) */
            kfree(f->post);
            f->post = NULL;
            f->post_len = 0;
        }
        kfree(f->body);
        f->body = NULL;
        f->cap = 0;
        begin(f);
        return;
    }
    if (!f->body) { /* (an empty body: still something to show) */
        f->body = (uint8_t *)kmalloc(1);
        if (f->body)
            f->body[0] = 0;
    }
    f->state = F_DONE;
}

int fetch_step(wv_fetch_t *f) {
    uint32_t now = time_millis();
    switch (f->state) {
    case F_DNS: {
        int r = dns_poll(f->dns, f->ip);
        if (r < 0) {
            f->dns = -1;
            fail(f, "Could not find that site (the name didn't resolve).");
        } else if (r > 0) {
            f->dns = -1;
            f->sock = tcp_open(f->ip, f->port, 0);
            if (f->sock < 0)
                fail(f, "Could not connect.");
            else {
                f->state = F_CONNECT;
                f->since = now;
            }
        }
        break;
    }
    case F_CONNECT: {
        tcp_state_t st = tcp_state(f->sock);
        if (st == TCP_ESTABLISHED) {
            f->state = F_SEND;
            f->since = now;
        } else if (st == TCP_CLOSED || tcp_was_reset(f->sock)) {
            fail(f, "The site refused the connection.");
        } else if (now - f->since > CONNECT_MS) {
            fail(f, "The site did not answer.");
        }
        break;
    }
    case F_SEND: {
        int n = tcp_write(f->sock, (const uint8_t *)f->req + f->req_sent,
                          (uint16_t)(f->req_len - f->req_sent > 4096
                                         ? 4096
                                         : f->req_len - f->req_sent));
        if (n < 0) {
            fail(f, "The connection broke.");
            break;
        }
        f->req_sent += n;
        if (f->req_sent >= f->req_len) {
            f->state = F_RECV;
            f->since = now;
        }
        break;
    }
    case F_RECV: {
        static uint8_t buf[2048];
        for (int round = 0; round < 16; round++) {
            int n = tcp_recv(f->sock, buf, sizeof(buf));
            if (n <= 0)
                break;
            got(f, buf, (uint32_t)n);
            f->since = now;
            if (f->truncated || finished(f))
                break;
        }
        tcp_state_t st = tcp_state(f->sock);
        bool open =
            st == TCP_ESTABLISHED || st == TCP_FIN_WAIT1 || st == TCP_FIN_WAIT2;
        if (f->truncated || finished(f) ||
            (!open && tcp_available(f->sock) == 0))
            complete(f);
        else if (now - f->since > IDLE_MS)
            fail(f, "The site stopped sending.");
        break;
    }
    default:
        break;
    }
    return f->state;
}

void fetch_status(const wv_fetch_t *f, char *out, int sz) {
    wv_url_t u;
    const char *host = url_split(f->url, &u) ? u.host : f->url;
    switch (f->state) {
    case F_DNS:
        snprintf(out, sz, "Looking up %s...", host);
        break;
    case F_CONNECT:
        snprintf(out, sz, "Connecting to %s...", host);
        break;
    case F_SEND:
        snprintf(out, sz, "Asking %s...", host);
        break;
    case F_RECV:
        if (f->clen > 0)
            snprintf(out, sz, "Reading: %u of %u KB", (unsigned)(f->len / 1024),
                     (unsigned)((uint32_t)f->clen / 1024));
        else
            snprintf(out, sz, "Reading: %u KB", (unsigned)(f->len / 1024));
        break;
    case F_FAILED:
        strlcpy(out, f->err, (size_t)sz);
        break;
    default:
        out[0] = 0;
    }
}
