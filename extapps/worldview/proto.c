// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* gopher:// and ftp://, a step a frame like HTTP (http.c runs them).
 *
 * Gopher: the selector sent, the answer read till the server closes. A
 * menu (type 1, and a search's answer) becomes a page: a line for each
 * item, a link where it leads. A search (type 7) with no words yet is a
 * page with a field for them. URLs: gopher://host[:port]/<type><selector>
 * (a search's words after '?').
 *
 * FTP: logged in (anonymous, or ftp://user:password@host/), TYPE I, then
 * CWD to the path: it works, a folder (its LIST becomes a page of
 * links); it doesn't, a file (RETR). Passive mode, to the server's own
 * address (the one in the PASV answer is often a private one). */

#include "extapps/worldview/wv.h"

void fetch_complete(wv_fetch_t *f);

#define FTP_IDLE_MS 30000

// a page being written (wv.h)

void bput_n(buf_t *b, const char *s, uint32_t n) {
    if (!b->p && b->cap)
        return; /* (out of memory before: stays empty) */
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n + 1)
            cap *= 2;
        char *nb = (char *)kmalloc(cap);
        if (!nb) {
            kfree(b->p);
            b->p = NULL;
            b->cap = 1;
            return;
        }
        if (b->p)
            memcpy(nb, b->p, b->n);
        kfree(b->p);
        b->p = nb;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void bput(buf_t *b, const char *s) { bput_n(b, s, (uint32_t)strlen(s)); }

/* text, its < > & " made entities */
void bput_text(buf_t *b, const char *s, int n) {
    for (int i = 0; i < n && s[i]; i++) {
        char c = s[i];
        if (c == '<')
            bput(b, "&lt;");
        else if (c == '>')
            bput(b, "&gt;");
        else if (c == '&')
            bput(b, "&amp;");
        else if (c == '"')
            bput(b, "&quot;");
        else if ((uint8_t)c >= 32 || c == '\t')
            bput_n(b, &c, 1);
    }
}

/* a path or selector in a URL: what would break it as %xx */
void bput_url(buf_t *b, const char *s, int n) {
    static const char HEX[] = "0123456789ABCDEF";
    for (int i = 0; i < n && s[i]; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c <= 32 || c >= 127 || c == '%' || c == '?' || c == '#' ||
            c == '"' || c == '<' || c == '>' || c == '+') {
            char e[3] = {'%', HEX[c >> 4], HEX[c & 15]};
            bput_n(b, e, 3);
        } else {
            bput_n(b, (const char *)&c, 1);
        }
    }
}

/* the page made: the fetch's body from now on */
static void become_page(wv_fetch_t *f, buf_t *b) {
    if (!b->p)
        return; /* (no memory: what came is shown as it is) */
    kfree(f->body);
    f->body = (uint8_t *)b->p;
    f->len = b->n;
    f->cap = b->cap;
    strcpy(f->ctype, "text/html");
}

// gopher

static const char *gopher_ctype(char t) {
    switch (t) {
    case '0':
        return "text/plain";
    case '1':
    case '7':
        return "text/gopher";
    case 'h':
        return "text/html";
    case 'g':
        return "image/gif";
    case 'I':
    case 'p':
    case ':':
        return "image/any";
    default:
        return "application/octet-stream";
    }
}

bool gopher_request(wv_fetch_t *f, const wv_url_t *u, char *out, int sz) {
    const char *p = u->path; /* "/", or "/<type><selector>" */
    char t = p[0] && p[1] ? p[1] : '1';
    char sel[WV_URL], query[200] = "";
    strlcpy(sel, p[0] && p[1] ? p + 2 : "", sizeof(sel));
    bool asked = false;
    if (t == '7') { /* a search: the words after '?' (q=, from the form) */
        char *q = strchr(sel, '?');
        if (q) {
            *q = 0;
            asked = true;
            strlcpy(query, q[1] == 'q' && q[2] == '=' ? q + 3 : q + 1,
                    sizeof(query));
            url_decode(query, true);
        }
    }
    url_decode(sel, false);
    f->gtype = t;
    strlcpy(f->ctype, gopher_ctype(t), sizeof(f->ctype));
    if (t == '7' && !asked) { /* the page asking for the words */
        buf_t b = {0};
        bput(&b, "<title>Search ");
        bput_text(&b, u->host, 80);
        bput(&b, "</title><h3>Gopher search</h3><p>");
        bput_text(&b, u->host, 80);
        bput(&b, " <tt>");
        bput_text(&b, sel, WV_URL);
        bput(&b, "</tt><form action=\"");
        bput_text(&b, f->url, WV_URL);
        bput(&b, "\">Search for: <input name=\"q\" size=\"30\"> <input "
                 "type=\"submit\" value=\"Search\"></form>");
        if (!b.p) {
            fetch_fail(f, "Not enough memory.");
            return true;
        }
        become_page(f, &b);
        return false;
    }
    if (asked)
        snprintf(out, sz, "%s\t%s\r\n", sel, query);
    else
        snprintf(out, sz, "%s\r\n", sel);
    return true;
}

/* one menu line: "<type><what is shown>\t<selector>\t<host>\t<port>" */
static void menu_line(buf_t *b, char *line) {
    char t = line[0];
    const char *field[4] = {line + 1, "", "", ""};
    int nf = 1;
    for (char *q = line + 1; *q && nf < 4; q++)
        if (*q == '\t') {
            *q = 0;
            field[nf++] = q + 1;
        }
    char *tab = strchr(field[3], '\t'); /* (gopher+: a '+' after the port) */
    if (tab)
        *tab = 0;
    const char *show = field[0], *sel = field[1], *host = field[2];
    int port = (int)str_to_uint32(field[3]);
    const char *label;
    switch (t) {
    case 'i':
        label = "      ";
        break;
    case '3':
        label = "ERR   ";
        break;
    case '0':
        label = "TXT   ";
        break;
    case '1':
        label = "DIR   ";
        break;
    case '7':
        label = "FIND  ";
        break;
    case 'h':
        label = "WEB   ";
        break;
    case 'g':
    case 'I':
    case 'p':
    case ':':
        label = "IMG   ";
        break;
    case 's':
    case '<':
        label = "SND   ";
        break;
    case '8':
    case 'T':
        label = "TEL   ";
        break;
    default:
        label = "BIN   ";
    }
    bput(b, label);
    bool link = t != 'i' && t != '3' && t != '8' && t != 'T' && host[0];
    if (!link) {
        bput_text(b, show, 200);
        bput(b, "\n");
        return;
    }
    bput(b, "<a href=\"");
    if (t == 'h' && !strncmp(sel, "URL:", 4)) { /* a link to the web */
        bput_text(b, sel + 4, WV_URL);
    } else {
        bput(b, "gopher://");
        bput_text(b, host, 80);
        if (port && port != 70) {
            char pt[8];
            snprintf(pt, sizeof(pt), ":%d", port);
            bput(b, pt);
        }
        char ty[3] = {'/', t, 0};
        bput(b, ty);
        bput_url(b, sel, WV_URL);
    }
    bput(b, "\">");
    bput_text(b, show, 200);
    bput(b, "</a>\n");
}

void gopher_done(wv_fetch_t *f) {
    if (!f->body || f->download)
        return;
    char *s = (char *)f->body;
    if (f->gtype == '0') { /* text: without the "." line at its end */
        int n = (int)f->len;
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
            n--;
        if (n && s[n - 1] == '.' && (n == 1 || s[n - 2] == '\n')) {
            n--;
            while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
                n--;
            f->len = (uint32_t)n;
            s[n] = 0;
        }
        return;
    }
    if (f->gtype != '1' && f->gtype != '7')
        return;
    wv_url_t u;
    url_split(f->url, &u);
    buf_t b = {0};
    bput(&b, "<title>");
    bput_text(&b, u.host, 80);
    if (u.path[0] && u.path[1] && u.path[2]) {
        bput(&b, " ");
        bput_text(&b, u.path + 2, 120);
    }
    bput(&b, "</title><pre>");
    uint32_t i = 0;
    while (i < f->len) {
        uint32_t e = i;
        while (e < f->len && s[e] != '\n')
            e++;
        uint32_t n = e - i;
        if (n && s[i + n - 1] == '\r')
            n--;
        char line[512];
        if (n > sizeof(line) - 1)
            n = sizeof(line) - 1;
        memcpy(line, s + i, n);
        line[n] = 0;
        i = e + 1;
        if (!strcmp(line, "."))
            break;
        if (n)
            menu_line(&b, line);
    }
    bput(&b, "</pre>");
    become_page(f, &b);
}

// ftp

static void ftp_send(wv_fetch_t *f, const char *cmd, const char *arg) {
    char line[WV_URL + 16];
    int n = snprintf(line, sizeof(line), arg ? "%s %s\r\n" : "%s\r\n", cmd,
                     arg ? arg : "");
    if (f->sock >= 0)
        tcp_write(f->sock, (const uint8_t *)line, (uint16_t)n);
    f->fcode = 0;
}

/* the reply's code when a whole one came (its last line's text in
 * f->fline), 0 not yet */
static int ftp_reply(wv_fetch_t *f) {
    uint8_t c;
    while (tcp_recv(f->sock, &c, 1) == 1) {
        f->since = time_millis();
        if (c == '\r')
            continue;
        if (c != '\n') {
            if (f->fline_len < (int)sizeof(f->fline) - 1)
                f->fline[f->fline_len++] = (char)c;
            continue;
        }
        f->fline[f->fline_len] = 0;
        int len = f->fline_len;
        f->fline_len = 0;
        const char *l = f->fline;
        bool coded = len >= 3 && l[0] >= '1' && l[0] <= '5' && l[1] >= '0' &&
                     l[1] <= '9' && l[2] >= '0' && l[2] <= '9';
        int code = coded ? (int)str_to_uint32(l) : 0;
        if (coded && l[3] == '-' && !f->fmulti) {
            f->fmulti = code; /* (a reply of several lines) */
        } else if (coded && (l[3] == ' ' || !l[3]) &&
                   (!f->fmulti || f->fmulti == code)) {
            f->fmulti = 0;
            return code;
        }
    }
    return 0;
}

/* the reply as an error */
static void ftp_refused(wv_fetch_t *f, const char *what) {
    char why[96];
    const char *msg = f->fline[0] ? f->fline + 3 : "";
    while (*msg == ' ' || *msg == '-')
        msg++;
    snprintf(why, sizeof(why), "%s: %s", what, msg[0] ? msg : "no answer");
    fetch_fail(f, why);
}

const char *wv_ctype(const char *path) {
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash))
        return "text/plain"; /* (README, INDEX) */
    static const char *const TEXT[] = {"TXT", "MD",  "C",   "H",   "ASM",
                                       "INI", "CFG", "LOG", "NFO", "DIZ",
                                       "BAT", "1ST", "ME",  NULL};
    for (int k = 0; TEXT[k]; k++)
        if (!strcasecmp(dot + 1, TEXT[k]))
            return "text/plain";
    if (!strcasecmp(dot, ".HTM") || !strcasecmp(dot, ".HTML"))
        return "text/html";
    if (!strcasecmp(dot, ".GIF"))
        return "image/gif";
    if (!strcasecmp(dot, ".JPG") || !strcasecmp(dot, ".JPEG"))
        return "image/jpeg";
    if (!strcasecmp(dot, ".BMP"))
        return "image/bmp";
    return "application/octet-stream";
}

enum {
    FS_GREET,
    FS_USER,
    FS_PASS,
    FS_TYPE,
    FS_CWD,
    FS_PASV,
    FS_DATA,
    FS_XFER,
};

static void ftp_path(const wv_fetch_t *f, char *out, int sz) {
    wv_url_t u;
    url_split(f->url, &u);
    strlcpy(out, u.path[0] ? u.path : "/", (size_t)sz);
    char *q = strchr(out, '?');
    if (q)
        *q = 0;
    url_decode(out, false);
}

void ftp_begin(wv_fetch_t *f) {
    f->state = F_RECV;
    f->fstep = FS_GREET;
    f->since = time_millis();
}

void ftp_free(wv_fetch_t *f) {
    if (f->data >= 0)
        tcp_abort(f->data);
    f->data = -1;
}

/* a folder's LIST: a page of links */
static void listing(wv_fetch_t *f) {
    wv_url_t u;
    url_split(f->url, &u);
    char path[WV_URL];
    ftp_path(f, path, sizeof(path));
    buf_t b = {0};
    bput(&b, "<title>");
    bput_text(&b, u.host, 80);
    bput(&b, " ");
    bput_text(&b, path, WV_URL);
    bput(&b, "</title><h3>");
    bput_text(&b, path, WV_URL);
    bput(&b, " on ");
    bput_text(&b, u.host, 80);
    bput(&b, "</h3><pre>");
    if (strcmp(path, "/"))
        bput(&b, "<a href=\"../\">[Up a folder]</a>\n");
    char *s = (char *)f->body;
    uint32_t i = 0;
    int files = 0;
    while (s && i < f->len) {
        uint32_t e = i;
        while (e < f->len && s[e] != '\n')
            e++;
        uint32_t n = e - i;
        if (n && s[i + n - 1] == '\r')
            n--;
        char line[400];
        if (n > sizeof(line) - 1)
            n = sizeof(line) - 1;
        memcpy(line, s + i, n);
        line[n] = 0;
        i = e + 1;
        if (!n || !strncmp(line, "total ", 6))
            continue;
        /* the fields: Unix ("drwxr-xr-x 1 o g size Mon dd hh:mm name"),
         * DOS ("01-31-96 10:00AM <DIR> name" or "... size name"), or
         * just the name */
        char *fld[9];
        int nf = 0;
        char *q = line;
        int want = (line[0] == 'd' || line[0] == '-' || line[0] == 'l')   ? 8
                   : (line[0] >= '0' && line[0] <= '9' && line[2] == '-') ? 3
                                                                          : 0;
        while (nf < want && *q) {
            while (*q == ' ')
                q++;
            fld[nf++] = q;
            while (*q && *q != ' ')
                q++;
            if (*q)
                *q++ = 0;
        }
        while (*q == ' ' && want)
            q++;
        char *name = nf == want ? q : line;
        if (nf != want)
            want = 0;
        bool dir = (want == 8 && line[0] == 'd') ||
                   (want == 3 && !strcmp(fld[2], "<DIR>"));
        const char *size = want == 8 ? fld[4] : want == 3 && !dir ? fld[2] : "";
        if (want == 8 && line[0] == 'l') { /* a link: "name -> where" */
            char *arrow = strstr(name, " -> ");
            if (arrow)
                *arrow = 0;
        }
        if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        bput(&b, "<a href=\"");
        bput_url(&b, name, 200);
        if (dir)
            bput(&b, "/");
        bput(&b, "\">");
        bput_text(&b, name, 200);
        int w = (int)strlen(name);
        if (dir) {
            bput(&b, "/");
            w++;
        }
        bput(&b, "</a>");
        if (size[0]) {
            char pad[48];
            int sp = w < 32 ? 32 - w : 1;
            int k = 0;
            while (k < sp && k < (int)sizeof(pad) - 1)
                pad[k++] = ' ';
            pad[k] = 0;
            bput(&b, pad);
            bput_text(&b, size, 16);
        }
        bput(&b, "\n");
        files++;
    }
    if (!files)
        bput(&b, "(nothing here)\n");
    bput(&b, "</pre>");
    become_page(f, &b);
}

static void ftp_finish(wv_fetch_t *f) {
    if (f->sock >= 0)
        ftp_send(f, "QUIT", NULL);
    ftp_free(f);
    if (f->fdir && !f->download)
        listing(f);
    fetch_complete(f);
}

void ftp_step(wv_fetch_t *f) {
    uint32_t now = time_millis();
    if (f->fstep == FS_XFER) {
        /* the data, and the server's word that it's all there */
        if (!f->ctl_done && f->sock >= 0) {
            int code = ftp_reply(f);
            if (code == 150 || code == 125) {
                const char *p = strchr(f->fline, '(');
                if (p && !f->fdir)
                    f->clen = (int32_t)str_to_uint32(p + 1);
            } else if (code >= 400) {
                ftp_refused(f, f->fdir ? "Can't list the folder"
                                       : "Can't get the file");
                return;
            } else if (code >= 200) {
                f->ctl_done = true;
            }
        }
        bool hold = f->download && !f->save[0] && f->len >= DL_HOLD;
        if (hold) {
            f->since = now;
            return;
        }
        static uint8_t buf[2048];
        for (int round = 0; round < 16 && f->data >= 0 && !hold; round++) {
            int n = tcp_recv(f->data, buf, sizeof(buf));
            if (n <= 0)
                break;
            fetch_keep(f, buf, (uint32_t)n);
            if (f->state != F_RECV)
                return;
            f->since = now;
            hold = f->download && !f->save[0] && f->len >= DL_HOLD;
        }
        if (hold)
            return;
        tcp_state_t st = tcp_state(f->data);
        bool open =
            st == TCP_ESTABLISHED || st == TCP_FIN_WAIT1 || st == TCP_FIN_WAIT2;
        if (!f->data_done && !open && tcp_available(f->data) == 0) {
            f->data_done = true;
            f->since = now;
        }
        /* all of it: the server said so (or said nothing for 3 s) */
        if (f->data_done && (f->ctl_done || now - f->since > 3000))
            ftp_finish(f);
        else if (now - f->since > FTP_IDLE_MS)
            fetch_fail(f, "The server stopped sending.");
        return;
    }
    if (f->fstep == FS_DATA) {
        tcp_state_t st = tcp_state(f->data);
        if (st == TCP_ESTABLISHED) {
            char path[WV_URL];
            ftp_path(f, path, sizeof(path));
            if (f->fdir) {
                ftp_send(f, "LIST", NULL);
                strcpy(f->ctype, "text/html");
            } else {
                ftp_send(f, "RETR", path);
                strlcpy(f->ctype, wv_ctype(path), sizeof(f->ctype));
                fetch_headed(f);
            }
            f->fstep = FS_XFER;
            f->since = now;
        } else if (st == TCP_CLOSED || tcp_was_reset(f->data)) {
            fetch_fail(f, "The server's data connection was refused.");
        } else if (now - f->since > FTP_IDLE_MS) {
            fetch_fail(f, "The server's data connection didn't open.");
        }
        return;
    }
    int code = ftp_reply(f);
    if (!code) {
        tcp_state_t st = tcp_state(f->sock);
        if (st == TCP_CLOSED || tcp_was_reset(f->sock))
            fetch_fail(f, "The server closed the connection.");
        else if (now - f->since > FTP_IDLE_MS)
            fetch_fail(f, "The server did not answer.");
        return;
    }
    wv_url_t u;
    url_split(f->url, &u);
    char path[WV_URL];
    switch (f->fstep) {
    case FS_GREET:
        if (code == 120) /* (wait: a 220 comes) */
            return;
        if (code != 220) {
            ftp_refused(f, "The server won't talk");
            return;
        }
        ftp_send(f, "USER", u.user[0] ? u.user : "anonymous");
        f->fstep = FS_USER;
        break;
    case FS_USER:
    case FS_PASS:
        if (f->fstep == FS_USER && (code == 331 || code == 332)) {
            ftp_send(f, "PASS", u.user[0] ? u.pass : "worldview@asmos");
            f->fstep = FS_PASS;
            break;
        }
        if (code != 230 && code != 202) { /* (230 to USER: no password) */
            ftp_refused(f, "Not let in");
            return;
        }
        ftp_send(f, "TYPE", "I");
        f->fstep = FS_TYPE;
        break;
    case FS_TYPE:
        ftp_path(f, path, sizeof(path));
        ftp_send(f, "CWD", path);
        f->fstep = FS_CWD;
        break;
    case FS_CWD: {
        ftp_path(f, path, sizeof(path));
        int n = (int)strlen(path);
        f->fdir = code >= 200 && code < 300;
        if (!f->fdir && n && path[n - 1] == '/') {
            ftp_refused(f, "No such folder");
            return;
        }
        if (f->fdir && (int)strlen(f->url) < WV_URL - 1) {
            /* a folder: its URL ends in '/' (links in it go under it) */
            char *q = strchr(f->url, '?');
            int un = q ? (int)(q - f->url) : (int)strlen(f->url);
            if (un && f->url[un - 1] != '/') {
                f->url[un] = '/';
                f->url[un + 1] = 0;
            }
        }
        ftp_send(f, "PASV", NULL);
        f->fstep = FS_PASV;
        break;
    }
    case FS_PASV: {
        /* "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" */
        const char *p = strchr(f->fline, '(');
        int v[6] = {0}, k = 0;
        for (p = p ? p + 1 : NULL; p && k < 6; k++) {
            v[k] = (int)str_to_uint32(p);
            while (*p >= '0' && *p <= '9')
                p++;
            if (*p != ',')
                break;
            p++;
        }
        if (code != 227 || k < 5) {
            ftp_refused(f, "No passive mode");
            return;
        }
        uint16_t port = (uint16_t)(v[4] * 256 + v[5]);
        f->data = tcp_open(f->ip, port, 0);
        if (f->data < 0) {
            fetch_fail(f, "Could not open the data connection.");
            return;
        }
        f->fstep = FS_DATA;
        f->since = now;
        break;
    }
    }
}
