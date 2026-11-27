// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* URLs, and HTTP/1.0 without waiting: fetch_step moves it on a step each
 * frame (look the name up, connect, send, read), so the window stays
 * alive. Redirects are followed (5 at most), chunked answers put back
 * together. Through a proxy ("host:port") the whole URL is asked for,
 * https:// too: the proxy does the encryption. gopher:// and ftp:// go
 * the same way, straight to the server (proto.c); file:/path is read from
 * the disk. A download (what isn't a page) waits at DL_HOLD bytes for its
 * file, then goes into it. */

#include "extapps/worldview/wv.h"

wv_net_cfg_t g_wv_net;

#define CONNECT_MS 15000
#define IDLE_MS 30000

// URLs

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static const struct {
    const char *name;
    uint16_t port;
} SCHEME[6] = {{"http", 80}, {"https", 443}, {"gopher", 70},
               {"ftp", 21},  {"file", 0},    {"about", 0}};

bool url_split(const char *url, wv_url_t *u) {
    memset(u, 0, sizeof(*u));
    const char *p = url;
    if (!strncasecmp(p, "about:", 6)) { /* about:bookmarks */
        u->scheme = SCH_ABOUT;
        strlcpy(u->path, p + 6, sizeof(u->path));
        return true;
    }
    if (!strncasecmp(p, "file:", 5)) { /* file:/HDA/PAGE.HTM, file:///... */
        p += 5;
        while (p[0] == '/' && p[1] == '/')
            p++;
        u->scheme = SCH_FILE;
        snprintf(u->path, sizeof(u->path), "%s%s", *p == '/' ? "" : "/", p);
        char *hash = strchr(u->path, '#');
        if (hash)
            *hash = 0;
        return true;
    }
    u->scheme = -1;
    for (int k = 0; k < 4; k++) { /* (the network ones: file, about above) */
        int n = (int)strlen(SCHEME[k].name);
        if (!strncasecmp(p, SCHEME[k].name, (size_t)n) &&
            !strncmp(p + n, "://", 3)) {
            u->scheme = k;
            u->port = SCHEME[k].port;
            p += n + 3;
            break;
        }
    }
    if (u->scheme < 0)
        return false;
    u->https = u->scheme == SCH_HTTPS;
    /* user:password@ (ftp) */
    const char *at = p;
    while (*at && *at != '/' && *at != '@')
        at++;
    if (*at == '@') {
        int k = 0;
        for (; p < at && *p != ':'; p++)
            if (k < (int)sizeof(u->user) - 1)
                u->user[k++] = *p;
        u->user[k] = 0;
        k = 0;
        if (*p == ':')
            for (p++; p < at; p++)
                if (k < (int)sizeof(u->pass) - 1)
                    u->pass[k++] = *p;
        u->pass[k] = 0;
        url_decode(u->user, false);
        url_decode(u->pass, false);
        p = at + 1;
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
    wv_url_t b;
    if (url_split(rel, &b) || !strncasecmp(rel, "mailto:", 7) ||
        !url_split(base, &b)) { /* (absolute already) */
        strlcpy(out, rel, (size_t)sz);
        return;
    }
    char origin[128];
    const char *sch = SCHEME[b.scheme].name;
    bool std_port = b.port == SCHEME[b.scheme].port;
    if (b.scheme == SCH_FILE)
        strcpy(origin, "file:");
    else
        snprintf(origin, sizeof(origin), "%s://%s", sch, b.host);
    if (!std_port) {
        char pt[8];
        snprintf(pt, sizeof(pt), ":%u", b.port);
        strlcat(origin, pt, sizeof(origin));
    }
    char path[WV_URL];
    if (rel[0] == '/' && rel[1] == '/') { /* //host/path */
        snprintf(out, sz, "%s:%s", sch, rel);
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

void url_decode(char *s, bool plus) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)str_to_ulong(h, NULL, 16);
            s += 2;
        } else {
            *o++ = plus && *s == '+' ? ' ' : *s;
        }
    }
    *o = 0;
}

// fetching

void fetch_fail(wv_fetch_t *f, const char *why) {
    if (f->sock >= 0)
        tcp_abort(f->sock);
    if (f->dns >= 0)
        dns_cancel(f->dns);
    f->sock = f->dns = -1;
    ftp_free(f);
    strlcpy(f->err, why, sizeof(f->err));
    f->state = F_FAILED;
}

/* where to connect: the proxy, or the URL's host */
static bool target(const wv_fetch_t *f, wv_url_t *u, char *host, int hsz,
                   uint16_t *port) {
    if (!url_split(f->url, u))
        return false;
    if (g_wv_net.proxy[0] && u->scheme <= SCH_HTTPS) {
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

/* the name looked up next */
static void goto_dns(wv_fetch_t *f, const char *host, uint16_t port) {
    f->req_sent = 0;
    f->dns = dns_start(host);
    f->port = port;
    f->state = f->dns >= 0 ? F_DNS : F_FAILED;
    if (f->dns < 0)
        strcpy(f->err, "Could not look the name up.");
    f->since = time_millis();
}

char *(*g_wv_about)(const char *url, uint32_t *len, char *show_at, int sz);

/* about:...: World View's own page, made now */
static void about_page(wv_fetch_t *f) {
    char at[WV_URL];
    strlcpy(at, f->url, sizeof(at));
    uint32_t n = 0;
    char *h = g_wv_about ? g_wv_about(f->url, &n, at, sizeof(at)) : NULL;
    if (!h) {
        fetch_fail(f, "There is no such page.");
        return;
    }
    strlcpy(f->url, at, sizeof(f->url));
    kfree(f->body);
    f->body = (uint8_t *)h;
    f->len = f->total = f->cap = n;
    strcpy(f->ctype, "text/html");
    f->status = 200;
    f->head_done = true;
    f->state = F_DONE;
}

/* file:/path: read from the disk, all at once */
static void local_file(wv_fetch_t *f, char *path) {
    url_decode(path, false);
    app_dirent_t de;
    if (!A->file_info(path, &de) || (de.attr & APP_ATTR_DIR)) {
        fetch_fail(f, "There is no such file.");
        return;
    }
    strlcpy(f->ctype, wv_ctype(path), sizeof(f->ctype));
    if (!strcmp(f->ctype, "application/octet-stream")) {
        fetch_fail(f, "World View shows pages, texts and pictures: open "
                      "this file from FileF.");
        return;
    }
    uint32_t n = de.size < f->max ? de.size : f->max;
    f->body = (uint8_t *)kmalloc(n + 1);
    if (!f->body) {
        fetch_fail(f, "Not enough memory.");
        return;
    }
    int got = n ? A->file_read(path, f->body, (int)n) : 0;
    if (got < 0) {
        fetch_fail(f, "Could not read the file.");
        return;
    }
    f->len = f->total = f->cap = (uint32_t)got;
    f->body[f->len] = 0;
    f->truncated = de.size > f->max;
    f->status = 200;
    f->head_done = true;
    f->state = F_DONE;
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
    f->total = 0;
    f->download = false;
    f->data = -1;
    f->fstep = f->fline_len = f->fmulti = f->fcode = 0;
    f->fdir = f->data_done = f->ctl_done = false;
    if (url_split(f->url, &u) && u.scheme == SCH_FILE) {
        f->scheme = SCH_FILE;
        local_file(f, u.path);
        return;
    }
    if (u.scheme == SCH_ABOUT) {
        f->scheme = SCH_ABOUT;
        about_page(f);
        return;
    }
    if (!target(f, &u, host, sizeof(host), &port)) {
        fetch_fail(f, "That is not an address World View knows (http://, "
                      "gopher://, ftp://).");
        return;
    }
    f->scheme = u.scheme;
    if (u.https && !g_wv_net.proxy[0]) {
        fetch_fail(f, "Secure sites (https://) need a proxy: Options > Proxy.");
        return;
    }
    if (!net_ready()) {
        fetch_fail(f, "No network: see Settings > Network.");
        return;
    }
    kfree(f->req);
    f->req = (char *)kmalloc(WV_URL + 512 + (uint32_t)f->post_len);
    if (!f->req) {
        fetch_fail(f, "Not enough memory.");
        return;
    }
    if (u.scheme >= SCH_GOPHER) { /* no head: what it is, known now */
        f->head_done = true;
        f->status = 200;
        f->req_len = 0;
        if (u.scheme == SCH_GOPHER &&
            !gopher_request(f, &u, f->req, WV_URL + 512)) {
            f->state = F_DONE; /* (a search: the page asking for words) */
            return;
        }
        if (f->state == F_FAILED)
            return;
        f->req_len = (int)strlen(f->req);
        goto_dns(f, host, port);
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
    goto_dns(f, host, port);
}

void fetch_start(wv_fetch_t *f, const char *url, const char *post, int post_len,
                 uint32_t max) {
    fetch_free(f);
    memset(f, 0, sizeof(*f));
    f->sock = f->dns = f->data = -1;
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
    ftp_free(f);
    kfree(f->req);
    kfree(f->post);
    kfree(f->body);
    f->req = f->post = NULL;
    f->body = NULL;
    f->len = f->cap = 0;
    if (f->state != F_DONE && f->state != F_FAILED)
        f->state = F_IDLE;
}

/* what is kept of a download: into its file */
static void flush(wv_fetch_t *f) {
    if (f->save[0] && f->len && !f->save_failed)
        f->save_failed = !A->file_append(f->save, f->body, (int)f->len);
    if (f->save[0])
        f->len = 0;
}

bool fetch_save_to(wv_fetch_t *f, const char *path) {
    if (!A->file_write(path, "", 0))
        return false;
    strlcpy(f->save, path, sizeof(f->save));
    flush(f);
    return !f->save_failed;
}

/* n more bytes of the body */
void fetch_keep(wv_fetch_t *f, const uint8_t *b, uint32_t n) {
    f->total += n;
    if (f->download) { /* (no end: held, or flushed into the file) */
        if (f->len + n > f->cap) {
            uint32_t cap = DL_HOLD + 4096;
            uint8_t *nb = (uint8_t *)kmalloc(cap + 1);
            if (!nb) {
                fetch_fail(f, "Not enough memory.");
                return;
            }
            if (f->body)
                memcpy(nb, f->body, f->len);
            kfree(f->body);
            f->body = nb;
            f->cap = cap;
        }
        if (f->len + n > f->cap) { /* (more than one read past the hold) */
            flush(f);
            if (f->len + n > f->cap) {
                fetch_fail(f, "The download overran.");
                return;
            }
        }
        memcpy(f->body + f->len, b, n);
        f->len += n;
        if (f->save[0] && f->len >= 16384)
            flush(f);
        return;
    }
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
        fetch_keep(f, b, n);
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
            fetch_keep(f, b + i, take);
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
            fetch_headed(f);
        }
    }
    if (i < n)
        body_bytes(f, b + i, n - i);
}

static bool finished(const wv_fetch_t *f) {
    if (!f->head_done || f->scheme >= SCH_GOPHER) /* (those: till closed) */
        return false;
    if (f->chunked)
        return f->chunk_st == 3;
    return f->clen >= 0 && f->total >= (uint32_t)f->clen;
}

/* is it a page (text, HTML, a picture) or a file to keep? */
void fetch_headed(wv_fetch_t *f) {
    if ((!f->may_download && !f->save_any) || f->status != 200 || f->download)
        return;
    if (f->save_any)
        f->download = true;
    const char *ct = f->ctype;
    bool pic = !strncmp(ct, "image/", 6);
    bool page = !ct[0] || !strncmp(ct, "text/", 5) || strstr(ct, "html") ||
                strstr(ct, "xml") || pic;
    uint32_t most = pic ? WV_IMG_FILE_MAX : f->max;
    if (!page || (f->clen > 0 && (uint32_t)f->clen > most && pic))
        f->download = true;
    if (f->download && f->len) { /* (what came with the head) */
        uint8_t *b = f->body;
        uint32_t n = f->len;
        f->body = NULL;
        f->len = f->cap = 0;
        f->total = 0;
        fetch_keep(f, b, n);
        kfree(b);
    }
}

/* done: a redirect starts again, else it is DONE */
void fetch_complete(wv_fetch_t *f);
void fetch_complete(wv_fetch_t *f) {
    if (f->sock >= 0)
        tcp_release(f->sock);
    f->sock = -1;
    if (!f->head_done) {
        fetch_fail(f, "The server sent nothing back.");
        return;
    }
    bool redirect = f->status == 301 || f->status == 302 || f->status == 303 ||
                    f->status == 307 || f->status == 308;
    if (redirect && f->location[0]) {
        if (++f->redirects > 5) {
            fetch_fail(f, "Too many redirects.");
            return;
        }
        char next[WV_URL];
        url_join(f->url, f->location, next, sizeof(next));
        if (!strncasecmp(next, "file:", 5) || !strncasecmp(next, "about:", 6)) {
            /* (a site can't read files, or work World View's own pages) */
            fetch_fail(f, "The site sent it to a file on this computer: "
                          "not followed.");
            return;
        }
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
    if (f->download) {
        flush(f);
    } else if (f->scheme == SCH_GOPHER) {
        gopher_done(f);
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
            fetch_fail(f,
                       "Could not find that site (the name didn't resolve).");
        } else if (r > 0) {
            f->dns = -1;
            f->sock = tcp_open(f->ip, f->port, 0);
            if (f->sock < 0)
                fetch_fail(f, "Could not connect.");
            else {
                f->state = F_CONNECT;
                f->since = now;
            }
        }
        break;
    }
    case F_CONNECT: {
        tcp_state_t st = tcp_state(f->sock);
        if (st == TCP_ESTABLISHED && f->scheme == SCH_FTP) {
            ftp_begin(f);
        } else if (st == TCP_ESTABLISHED) {
            f->state = F_SEND;
            f->since = now;
        } else if (st == TCP_CLOSED || tcp_was_reset(f->sock)) {
            fetch_fail(f, "The site refused the connection.");
        } else if (now - f->since > CONNECT_MS) {
            fetch_fail(f, "The site did not answer.");
        }
        break;
    }
    case F_SEND: {
        int n = tcp_write(f->sock, (const uint8_t *)f->req + f->req_sent,
                          (uint16_t)(f->req_len - f->req_sent > 4096
                                         ? 4096
                                         : f->req_len - f->req_sent));
        if (n < 0) {
            fetch_fail(f, "The connection broke.");
            break;
        }
        f->req_sent += n;
        if (f->req_sent >= f->req_len) {
            f->state = F_RECV;
            f->since = now;
            if (f->scheme == SCH_GOPHER)
                fetch_headed(f);
        }
        break;
    }
    case F_RECV: {
        if (f->scheme == SCH_FTP) {
            ftp_step(f);
            break;
        }
        static uint8_t buf[2048];
        if (f->download && !f->save[0] && f->len >= DL_HOLD) {
            f->since = now; /* (waits for its file: the server waits too) */
            break;
        }
        for (int round = 0; round < 16; round++) {
            int n = tcp_recv(f->sock, buf, sizeof(buf));
            if (n <= 0)
                break;
            got(f, buf, (uint32_t)n);
            f->since = now;
            if (f->truncated || finished(f) || f->state != F_RECV ||
                (f->download && !f->save[0] && f->len >= DL_HOLD))
                break;
        }
        if (f->state != F_RECV)
            break;
        tcp_state_t st = tcp_state(f->sock);
        bool open =
            st == TCP_ESTABLISHED || st == TCP_FIN_WAIT1 || st == TCP_FIN_WAIT2;
        if (f->download && !f->save[0] && f->len >= DL_HOLD)
            break;
        if (f->truncated || finished(f) ||
            (!open && tcp_available(f->sock) == 0))
            fetch_complete(f);
        else if (now - f->since > IDLE_MS)
            fetch_fail(f, "The site stopped sending.");
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
        if (f->scheme == SCH_FTP && f->data < 0)
            snprintf(out, sz, "Talking to %s...", host);
        else if (f->clen > 0)
            snprintf(out, sz, "Reading: %u of %u KB",
                     (unsigned)(f->total / 1024),
                     (unsigned)((uint32_t)f->clen / 1024));
        else
            snprintf(out, sz, "Reading: %u KB", (unsigned)(f->total / 1024));
        break;
    case F_FAILED:
        strlcpy(out, f->err, (size_t)sz);
        break;
    default:
        out[0] = 0;
    }
}
