// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "network/ftpd.h"
#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "network/net.h"
#include "os/scheduler.h"

#define IDLE_MS (10 * 60 * 1000) /* a quiet client is let go */
#define DATA_WAIT_MS 30000       /* for the data connection to come */
#define BUF 2048
#define PASV_LO 50000 /* the data ports: few, so they can be forwarded */
#define PASV_HI 50009

enum { X_NONE, X_WAIT, X_SEND, X_RECV }; /* a transfer's stage */

typedef struct {
    bool used;
    int ctl;
    char lb[512];
    uint16_t ln;
    bool user_ok, logged;
    char cwd[FS_PATH_SZ]; /* as the client sees it: "/", "/HDA/GAMES" */
    uint32_t rest;
    char rnfr[FS_PATH_SZ];
    uint32_t idle_t;
    /* the data connection: passive (a listener), or active (PORT) */
    int pasv, data;
    bool active;
    uint8_t act_ip[4];
    uint16_t act_port;
    /* a transfer */
    int xfer;
    bool to_client;
    uint32_t xfer_t;
    fat_file_t f;
    bool file_open;
    uint8_t *mem; /* a listing to send */
    uint32_t mem_len, mem_off;
    uint8_t buf[BUF];
    uint16_t blen, boff;
} sess_t;

static sess_t *s_sess; /* [FTPD_SESSIONS], while running */
static int s_listen = -1, s_task = -1;
static bool s_running, s_ro;
static uint16_t s_port;
static char s_pass[32];
static uint32_t s_sent, s_got;

bool ftpd_running(void) { return s_running; }
uint16_t ftpd_port(void) { return s_port; }
bool ftpd_read_only(void) { return s_ro; }
uint32_t ftpd_files_sent(void) { return s_sent; }
uint32_t ftpd_files_got(void) { return s_got; }

int ftpd_clients(void) {
    int n = 0;
    for (int i = 0; s_sess && i < FTPD_SESSIONS; i++)
        n += s_sess[i].used;
    return n;
}

static void say(sess_t *s, const char *fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b) - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 3)
        n = (int)sizeof(b) - 3;
    b[n++] = '\r';
    b[n++] = '\n';
    tcp_send(s->ctl, (const uint8_t *)b, (uint16_t)n);
}

// paths

/* the client's path made whole ("..", ".", relative to cwd): "/A/B" */
static bool ftp_path(const sess_t *s, const char *arg, char out[FS_PATH_SZ]) {
    char t[FS_PATH_SZ * 2];
    if (arg[0] == '/')
        snprintf(t, sizeof(t), "%s", arg);
    else
        snprintf(t, sizeof(t), "%s/%s", s->cwd, arg);
    int o = 0;
    const char *p = t;
    out[0] = 0;
    while (*p) {
        while (*p == '/')
            p++;
        const char *q = p;
        while (*q && *q != '/')
            q++;
        int n = (int)(q - p);
        if (n == 0)
            break;
        if (n == 1 && p[0] == '.') {
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
        } else {
            if (o + 1 + n >= FS_PATH_SZ)
                return false;
            out[o++] = '/';
            memcpy(out + o, p, n);
            o += n;
        }
        out[o] = 0;
        p = q;
    }
    if (!o)
        strcpy(out, "/");
    return true;
}

/* the drive a path starts with ("/HDB/X": HDB), -1: none (the root) */
static int path_drive(const char *p, const char **rest) {
    for (int d = 0; d < DRIVE_COUNT; d++) {
        const char *n = g_drive_paths[d];
        if (!n || d == DRIVE_NET0)
            continue;
        size_t l = strlen(n);
        if (!strncasecmp(p, n, l) && (p[l] == 0 || p[l] == '/')) {
            *rest = p + l;
            return fs_drive_mounted(d) ? d : -2;
        }
    }
    return -1;
}

/* the path here: "/HDA/X" -> "/X", "/HDB/X" -> "/HDB/X"; false: not a
 * file or folder of a drive ("/" itself, "/NOPE") */
static bool local_path(const char *fp, char out[FS_PATH_SZ]) {
    const char *rest;
    int d = path_drive(fp, &rest);
    if (d < 0)
        return false;
    if (d == DRIVE_HDA)
        snprintf(out, FS_PATH_SZ, "%s", *rest ? rest : "/");
    else
        snprintf(out, FS_PATH_SZ, "%s%s", g_drive_paths[d], rest);
    return true;
}

static bool is_dir(const char *fp) {
    if (!strcmp(fp, "/"))
        return true;
    char lp[FS_PATH_SZ];
    uint8_t d;
    uint16_t c;
    return local_path(fp, lp) && fs_resolve_dir(lp, &d, &c);
}

// listings

typedef struct {
    sess_t *s;
    int kind; /* 0: LIST, 1: NLST, 2: MLSD */
    uint32_t cap;
    bool ok;
} list_ctx;

static void put(list_ctx *c, const char *t) {
    sess_t *s = c->s;
    uint32_t n = (uint32_t)strlen(t);
    if (s->mem_len + n > c->cap) {
        uint32_t cap = (c->cap + n) * 2;
        uint8_t *m = kmalloc(cap);
        if (!m) {
            c->ok = false;
            return;
        }
        memcpy(m, s->mem, s->mem_len);
        kfree(s->mem);
        s->mem = m;
        c->cap = cap;
    }
    memcpy(s->mem + s->mem_len, t, n);
    s->mem_len += n;
}

static const char *const k_mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void list_one(list_ctx *c, const char *name, bool dir, uint32_t size,
                     uint16_t date, uint16_t time, bool ro) {
    char b[200];
    int y = 1980 + (date >> 9), mo = (date >> 5) & 15, d = date & 31;
    int h = time >> 11, mi = (time >> 5) & 63, sec = (time & 31) * 2;
    if (mo < 1 || mo > 12)
        mo = 1;
    if (c->kind == 1) {
        snprintf(b, sizeof(b), "%s\r\n", name);
    } else if (c->kind == 2) {
        char sz[24] = "";
        if (!dir)
            snprintf(sz, sizeof(sz), "size=%u;", size);
        snprintf(b, sizeof(b),
                 "type=%s;%smodify=%04d%02d%02d%02d%02d%02d;perm=%s; %s\r\n",
                 dir ? "dir" : "file", sz, y, mo, d ? d : 1, h, mi, sec,
                 dir ? (ro ? "el" : "elcmfp") : (ro ? "r" : "rwdf"), name);
    } else {
        char when[16];
        if ((uint32_t)y == time_rtc_local().year)
            snprintf(when, sizeof(when), "%s %2d %02d:%02d", k_mon[mo - 1], d,
                     h, mi);
        else
            snprintf(when, sizeof(when), "%s %2d  %4d", k_mon[mo - 1], d, y);
        snprintf(b, sizeof(b), "%s 1 asmos asmos %10u %s %s\r\n",
                 dir ? (ro ? "dr-xr-xr-x" : "drwxr-xr-x")
                     : (ro ? "-r--r--r--" : "-rw-r--r--"),
                 size, when, name);
    }
    put(c, b);
}

static bool walk_cb(const dir_entry_t *e, const char *name, void *user) {
    list_ctx *c = user;
    if (e->attr & (ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID))
        return c->ok;
    list_one(c, name, e->attr & ATTR_DIRECTORY, e->file_size, e->modify_date,
             e->modify_time, (e->attr & ATTR_READ_ONLY) || s_ro);
    return c->ok;
}

/* the listing of fp into s->mem; false: no such folder */
static bool make_listing(sess_t *s, const char *fp, int kind) {
    list_ctx c = {s, kind, 0, true};
    s->mem_len = s->mem_off = 0;
    if (!strcmp(fp, "/")) { /* the drives */
        uint16_t today = date_from_rtc();
        for (int d = 0; d < DRIVE_COUNT; d++)
            if (d != DRIVE_NET0 && fs_drive_mounted(d))
                list_one(&c, g_drive_paths[d] + 1, true, 0, today, 0,
                         d == DRIVE_CD0 || s_ro);
        return c.ok;
    }
    char lp[FS_PATH_SZ];
    uint8_t d;
    uint16_t cl;
    if (!local_path(fp, lp))
        return false;
    if (fs_resolve_dir(lp, &d, &cl)) {
        fs_walk_dir(d, cl, walk_cb, &c);
        return c.ok;
    }
    dir_entry_t e; /* LIST of a file: that one */
    char nm[FS_NAME_SZ];
    if (!fs_find(lp, &e) || !fs_name_of(lp, nm))
        return false;
    list_one(&c, nm, e.attr & ATTR_DIRECTORY, e.file_size, e.modify_date,
             e.modify_time, (e.attr & ATTR_READ_ONLY) || s_ro);
    return c.ok;
}

// transfers

static void end_xfer(sess_t *s, bool cut) {
    if (s->file_open) {
        fs_close(&s->f);
        s->file_open = false;
    }
    kfree(s->mem);
    s->mem = NULL;
    if (s->data >= 0) {
        if (cut)
            tcp_abort(s->data);
        else
            tcp_release(s->data);
    }
    if (s->pasv >= 0)
        tcp_release(s->pasv);
    s->data = s->pasv = -1;
    s->active = false;
    s->xfer = X_NONE;
    s->blen = s->boff = 0;
}

/* a transfer can start: the data connection is there (or is made) */
static bool begin_xfer(sess_t *s, bool to_client) {
    if (s->pasv < 0 && !s->active) {
        say(s, "425 Use PASV or PORT first.");
        end_xfer(s, true);
        return false;
    }
    s->to_client = to_client;
    s->xfer = X_WAIT;
    s->xfer_t = time_millis();
    say(s, "150 Here it comes.");
    if (s->active) { /* (it waits: active is rare) */
        s->data = tcp_connect(s->act_ip, s->act_port, 0);
        if (s->data < 0) {
            say(s, "425 Can't open the data connection.");
            end_xfer(s, true);
            return false;
        }
    }
    return true;
}

static void finish(sess_t *s, bool ok, const char *why) {
    bool was_file = s->file_open;
    end_xfer(s, !ok);
    if (ok) {
        if (was_file)
            s->to_client ? s_sent++ : s_got++;
        say(s, "226 Done.");
    } else {
        say(s, "%s", why);
    }
}

/* moves what it can; called every round */
static void pump(sess_t *s) {
    if (s->xfer == X_WAIT) {
        if (s->data < 0 && s->pasv >= 0)
            s->data = tcp_accept(s->pasv);
        if (s->data < 0) {
            if (time_millis() - s->xfer_t > DATA_WAIT_MS)
                finish(s, false, "425 No data connection came.");
            return;
        }
        if (s->pasv >= 0) {
            tcp_release(s->pasv);
            s->pasv = -1;
        }
        s->xfer = s->to_client ? X_SEND : X_RECV;
    }
    if (s->xfer == X_SEND) {
        for (int round = 0; round < 8; round++) {
            if (s->boff == s->blen) { /* refill */
                s->boff = 0;
                if (s->mem) {
                    uint32_t n = s->mem_len - s->mem_off;
                    s->blen = (uint16_t)(n < BUF ? n : BUF);
                    memcpy(s->buf, s->mem + s->mem_off, s->blen);
                    s->mem_off += s->blen;
                } else {
                    int n = fs_read(&s->f, s->buf, BUF);
                    s->blen = n > 0 ? (uint16_t)n : 0;
                }
                if (!s->blen) {
                    finish(s, true, NULL);
                    return;
                }
            }
            int w = tcp_write(s->data, s->buf + s->boff,
                              (uint16_t)(s->blen - s->boff));
            if (w < 0) {
                finish(s, false, "426 The connection was lost.");
                return;
            }
            if (w == 0)
                return; /* (full: next round) */
            s->boff += (uint16_t)w;
        }
        return;
    }
    if (s->xfer == X_RECV) {
        for (int round = 0; round < 8; round++) {
            int r = tcp_recv(s->data, s->buf, BUF);
            if (r > 0) {
                if (fs_write(&s->f, s->buf, r) != r) {
                    finish(s, false, "552 The disk is full.");
                    return;
                }
                continue;
            }
            tcp_state_t st = tcp_state(s->data);
            if (st == TCP_ESTABLISHED)
                return;
            if (tcp_was_reset(s->data))
                finish(s, false, "426 The connection was lost.");
            else
                finish(s, true, NULL); /* (the client closed: all came) */
            return;
        }
    }
}

// commands

static bool pasv_open(sess_t *s) {
    static uint16_t next = PASV_LO;
    end_xfer(s, true);
    for (int k = 0; k <= PASV_HI - PASV_LO && s->pasv < 0; k++) {
        s->pasv = tcp_listen(next);
        next = next >= PASV_HI ? PASV_LO : next + 1;
    }
    return s->pasv >= 0;
}

/* "h1,h2,h3,h4,p1,p2" (PORT) */
static bool parse_port(sess_t *s, const char *a) {
    int v[6], n = 0;
    while (n < 6) {
        if (*a < '0' || *a > '9')
            return false;
        int x = 0;
        while (*a >= '0' && *a <= '9')
            x = x * 10 + (*a++ - '0');
        if (x > 255)
            return false;
        v[n++] = x;
        if (n < 6 && *a++ != ',')
            return false;
    }
    for (int i = 0; i < 4; i++)
        s->act_ip[i] = (uint8_t)v[i];
    s->act_port = (uint16_t)(v[4] << 8 | v[5]);
    return true;
}

/* "|1|10.0.2.2|6446|" (EPRT) */
static bool parse_eprt(sess_t *s, const char *a) {
    char d = *a;
    if (!d || a[1] != '1' || a[2] != d)
        return false;
    a += 3;
    for (int i = 0; i < 4; i++) {
        int x = 0, k = 0;
        while (*a >= '0' && *a <= '9' && k++ < 3)
            x = x * 10 + (*a++ - '0');
        if (!k || x > 255 || *a++ != (i < 3 ? '.' : d))
            return false;
        s->act_ip[i] = (uint8_t)x;
    }
    uint32_t p = 0;
    while (*a >= '0' && *a <= '9')
        p = p * 10 + (uint32_t)(*a++ - '0');
    if (*a != d || !p || p > 65535)
        return false;
    s->act_port = (uint16_t)p;
    return true;
}

/* the file a command names: its path here, or false (with a reply) */
static bool file_arg(sess_t *s, const char *arg, char fp[FS_PATH_SZ],
                     char lp[FS_PATH_SZ]) {
    if (!*arg || !ftp_path(s, arg, fp) || !local_path(fp, lp)) {
        say(s, "550 No such file.");
        return false;
    }
    return true;
}

/* fp may be changed: not read only, not on the CD */
static bool writable(sess_t *s, const char *fp) {
    const char *rest;
    if (s_ro || path_drive(fp, &rest) == DRIVE_CD0) {
        say(s, "550 Read only.");
        return false;
    }
    return true;
}

static void cmd_retr(sess_t *s, const char *arg) {
    char fp[FS_PATH_SZ], lp[FS_PATH_SZ];
    if (!file_arg(s, arg, fp, lp))
        return;
    dir_entry_t e;
    if (!fs_find(lp, &e) || (e.attr & ATTR_DIRECTORY) || !fs_open(lp, &s->f)) {
        say(s, "550 No such file.");
        return;
    }
    s->file_open = true;
    if (s->rest && !fs_seek(&s->f, s->rest)) {
        say(s, "554 Can't start there.");
        end_xfer(s, true);
        return;
    }
    begin_xfer(s, true);
}

static void cmd_stor(sess_t *s, const char *arg, bool append) {
    char fp[FS_PATH_SZ], lp[FS_PATH_SZ];
    if (!file_arg(s, arg, fp, lp) || !writable(s, fp))
        return;
    dir_entry_t e;
    bool there = fs_find(lp, &e);
    if (there && (e.attr & ATTR_DIRECTORY)) {
        say(s, "550 That's a folder.");
        return;
    }
    bool ok;
    if (there && (append || s->rest)) {
        ok = fs_open(lp, &s->f) &&
             fs_seek(&s->f,
                     append ? e.file_size
                            : (s->rest < e.file_size ? s->rest : e.file_size));
    } else {
        if (there && !fs_delete(lp)) {
            say(s, "550 Can't replace it.");
            return;
        }
        ok = fs_create(lp, &s->f);
    }
    if (!ok) {
        say(s, "553 Can't make that file.");
        return;
    }
    s->file_open = true;
    begin_xfer(s, false);
}

static void cmd_list(sess_t *s, const char *arg, int kind) {
    while (*arg == '-') { /* "LIST -la": options, ignored */
        while (*arg && *arg != ' ')
            arg++;
        while (*arg == ' ')
            arg++;
    }
    char fp[FS_PATH_SZ];
    if (!ftp_path(s, *arg ? arg : ".", fp) || !make_listing(s, fp, kind)) {
        kfree(s->mem);
        s->mem = NULL;
        say(s, "550 No such folder.");
        return;
    }
    if (!s->mem) /* (empty: still a buffer, so the pump knows) */
        s->mem = kmalloc(1);
    begin_xfer(s, true);
}

static void cmd_mlst(sess_t *s, const char *arg) {
    char fp[FS_PATH_SZ];
    if (!ftp_path(s, *arg ? arg : ".", fp)) {
        say(s, "550 No such file.");
        return;
    }
    list_ctx c = {s, 2, 0, true};
    s->mem_len = 0;
    char lp[FS_PATH_SZ];
    dir_entry_t e;
    if (is_dir(fp))
        list_one(&c, fp, true, 0, date_from_rtc(), 0, s_ro);
    else if (local_path(fp, lp) && fs_find(lp, &e))
        list_one(&c, fp, false, e.file_size, e.modify_date, e.modify_time,
                 s_ro);
    if (!s->mem_len) {
        say(s, "550 No such file.");
    } else {
        s->mem[s->mem_len - 2] = 0; /* (no CR LF: say() adds one) */
        say(s, "250-Listing %s", fp);
        say(s, " %s", (char *)s->mem);
        say(s, "250 End");
    }
    kfree(s->mem);
    s->mem = NULL;
}

static void cmd_cwd(sess_t *s, const char *arg) {
    char fp[FS_PATH_SZ];
    if (!ftp_path(s, *arg ? arg : "/", fp) || !is_dir(fp)) {
        say(s, "550 No such folder.");
        return;
    }
    strcpy(s->cwd, fp);
    say(s, "250 In \"%s\".", fp);
}

static void cmd_size_mdtm(sess_t *s, const char *arg, bool size) {
    char fp[FS_PATH_SZ], lp[FS_PATH_SZ];
    dir_entry_t e;
    if (!file_arg(s, arg, fp, lp))
        return;
    if (!fs_find(lp, &e) || (e.attr & ATTR_DIRECTORY)) {
        say(s, "550 No such file.");
        return;
    }
    if (size) {
        say(s, "213 %u", e.file_size);
        return;
    }
    uint16_t d = e.modify_date, t = e.modify_time;
    say(s, "213 %04d%02d%02d%02d%02d%02d", 1980 + (d >> 9), (d >> 5) & 15,
        d & 31, t >> 11, (t >> 5) & 63, (t & 31) * 2);
}

static void cmd_dele_rmd(sess_t *s, const char *arg, bool dir) {
    char fp[FS_PATH_SZ], lp[FS_PATH_SZ];
    if (!file_arg(s, arg, fp, lp) || !writable(s, fp))
        return;
    dir_entry_t e;
    bool ok = fs_find(lp, &e) && !(e.attr & ATTR_DIRECTORY) == !dir &&
              (dir ? fs_rmdir(lp) : fs_delete(lp));
    if (ok)
        say(s, "250 Gone.");
    else
        say(s, dir ? "550 No such folder, or not empty." : "550 No such file.");
}

static void cmd_mkd(sess_t *s, const char *arg) {
    char fp[FS_PATH_SZ], lp[FS_PATH_SZ];
    if (!file_arg(s, arg, fp, lp) || !writable(s, fp))
        return;
    if (!fs_mkdir(lp)) { /* (any name the disk takes: spaces too) */
        say(s, "550 Can't make that folder.");
        return;
    }
    say(s, "257 \"%s\" made.", fp);
}

static void cmd_rnto(sess_t *s, const char *arg) {
    char fp[FS_PATH_SZ], lp[FS_PATH_SZ], from[FS_PATH_SZ];
    if (!s->rnfr[0]) {
        say(s, "503 RNFR first.");
        return;
    }
    if (!file_arg(s, arg, fp, lp) || !local_path(s->rnfr, from)) {
        s->rnfr[0] = 0;
        return;
    }
    s->rnfr[0] = 0;
    dir_entry_t e;
    const char *a = strrchr(from, '/'), *b = strrchr(lp, '/');
    bool same_dir =
        a && b && a - from == b - lp && !strncmp(from, lp, a - from);
    bool ok = fs_find(from, &e);
    if (ok && same_dir)
        ok = fs_rename(from, b + 1);
    else if (ok)
        ok = e.attr & ATTR_DIRECTORY ? fs_move_dir(from, lp)
                                     : fs_move_file(from, lp);
    if (ok)
        say(s, "250 Renamed.");
    else
        say(s, "553 Can't rename it.");
}

static void command(sess_t *s, char *line) {
    char *arg = line;
    while (*arg && *arg != ' ')
        arg++;
    if (*arg)
        *arg++ = 0;
    for (char *p = line; *p; p++)
        *p = (char)toupper(*p);
    const char *c = line;
    s->idle_t = time_millis();

    if (!strcmp(c, "USER")) {
        s->user_ok = true;
        s->logged = !s_pass[0];
        if (s->logged)
            say(s, "230 Hello.");
        else
            say(s, "331 The password, please.");
        return;
    }
    if (!strcmp(c, "PASS")) {
        if (!s->user_ok) {
            say(s, "503 USER first.");
        } else if (s->logged || !strcmp(arg, s_pass)) {
            s->logged = true;
            say(s, "230 Hello.");
        } else {
            say(s, "530 Wrong password.");
        }
        return;
    }
    if (!strcmp(c, "QUIT")) {
        say(s, "221 Bye.");
        end_xfer(s, true);
        tcp_release(s->ctl);
        s->used = false;
        return;
    }
    if (!strcmp(c, "NOOP")) {
        say(s, "200 OK.");
        return;
    }
    if (!strcmp(c, "SYST")) {
        say(s, "215 UNIX Type: L8");
        return;
    }
    if (!strcmp(c, "FEAT")) {
        say(s, "211-Features:");
        say(s, " MDTM");
        say(s, " MLST type*;size*;modify*;perm*;");
        say(s, " PASV");
        say(s, " EPSV");
        say(s, " REST STREAM");
        say(s, " SIZE");
        say(s, "211 End");
        return;
    }
    if (!strcmp(c, "OPTS")) {
        say(s, "200 OK.");
        return;
    }
    if (!s->logged) {
        say(s, "530 Log in first.");
        return;
    }
    if (s->xfer != X_NONE && strcmp(c, "ABOR") && strcmp(c, "STAT")) {
        say(s, "425 A transfer is going on.");
        return;
    }
    uint32_t rest = s->rest;
    s->rest = 0; /* (REST is for the next command only) */

    if (!strcmp(c, "PWD") || !strcmp(c, "XPWD")) {
        say(s, "257 \"%s\" is where you are.", s->cwd);
    } else if (!strcmp(c, "CWD") || !strcmp(c, "XCWD")) {
        cmd_cwd(s, arg);
    } else if (!strcmp(c, "CDUP") || !strcmp(c, "XCUP")) {
        cmd_cwd(s, "..");
    } else if (!strcmp(c, "TYPE")) {
        char t = (char)toupper(arg[0]);
        if (t == 'I' || t == 'A' || t == 'L')
            say(s, "200 Binary is all there is.");
        else
            say(s, "504 Not that type.");
    } else if (!strcmp(c, "MODE")) {
        say(s, toupper(arg[0]) == 'S' ? "200 OK." : "504 Stream only.");
    } else if (!strcmp(c, "STRU")) {
        say(s, toupper(arg[0]) == 'F' ? "200 OK." : "504 Files only.");
    } else if (!strcmp(c, "PASV") || !strcmp(c, "EPSV")) {
        if (!pasv_open(s)) {
            say(s, "425 Too busy now, try again.");
            return;
        }
        uint16_t p = tcp_local_port(s->pasv);
        uint8_t ip[4];
        net_get_ip(ip);
        if (c[0] == 'E')
            say(s, "229 Entering Extended Passive Mode (|||%u|)", p);
        else
            say(s, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)", ip[0],
                ip[1], ip[2], ip[3], p >> 8, p & 0xFF);
    } else if (!strcmp(c, "PORT") || !strcmp(c, "EPRT")) {
        end_xfer(s, true);
        if (c[0] == 'P' ? parse_port(s, arg) : parse_eprt(s, arg)) {
            s->active = true;
            say(s, "200 OK.");
        } else {
            say(s, "501 That isn't an address.");
        }
    } else if (!strcmp(c, "REST")) {
        uint32_t v = 0;
        for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
            v = v * 10 + (uint32_t)(*p - '0');
        s->rest = v;
        say(s, "350 From byte %u.", v);
    } else if (!strcmp(c, "RETR")) {
        s->rest = rest;
        cmd_retr(s, arg);
        s->rest = 0;
    } else if (!strcmp(c, "STOR") || !strcmp(c, "APPE")) {
        s->rest = rest;
        cmd_stor(s, arg, c[0] == 'A');
        s->rest = 0;
    } else if (!strcmp(c, "LIST")) {
        cmd_list(s, arg, 0);
    } else if (!strcmp(c, "NLST")) {
        cmd_list(s, arg, 1);
    } else if (!strcmp(c, "MLSD")) {
        cmd_list(s, arg, 2);
    } else if (!strcmp(c, "MLST")) {
        cmd_mlst(s, arg);
    } else if (!strcmp(c, "SIZE") || !strcmp(c, "MDTM")) {
        cmd_size_mdtm(s, arg, c[0] == 'S');
    } else if (!strcmp(c, "DELE")) {
        cmd_dele_rmd(s, arg, false);
    } else if (!strcmp(c, "RMD") || !strcmp(c, "XRMD")) {
        cmd_dele_rmd(s, arg, true);
    } else if (!strcmp(c, "MKD") || !strcmp(c, "XMKD")) {
        cmd_mkd(s, arg);
    } else if (!strcmp(c, "RNFR")) {
        char fp[FS_PATH_SZ], lp[FS_PATH_SZ];
        dir_entry_t e;
        if (file_arg(s, arg, fp, lp) && writable(s, fp)) {
            if (fs_find(lp, &e)) {
                strcpy(s->rnfr, fp);
                say(s, "350 And the new name?");
            } else {
                say(s, "550 No such file.");
            }
        }
    } else if (!strcmp(c, "RNTO")) {
        cmd_rnto(s, arg);
    } else if (!strcmp(c, "ABOR")) {
        if (s->xfer != X_NONE) {
            end_xfer(s, true);
            say(s, "426 Stopped.");
        }
        say(s, "226 OK.");
    } else if (!strcmp(c, "STAT")) {
        say(s, "211 ASMOS FTP: %s.",
            s->xfer != X_NONE ? "a transfer is going on" : "ready");
    } else if (!strcmp(c, "ALLO")) {
        say(s, "202 No need.");
    } else if (!strcmp(c, "HELP")) {
        say(s, "214-Commands:");
        say(s, " USER PASS QUIT PWD CWD CDUP TYPE PASV EPSV PORT EPRT");
        say(s, " LIST NLST MLSD MLST RETR STOR APPE REST SIZE MDTM");
        say(s, " DELE MKD RMD RNFR RNTO ABOR NOOP SYST FEAT STAT");
        say(s, "214 OK.");
    } else {
        say(s, "502 Not that one.");
    }
}

// the server

static void session_input(sess_t *s) {
    int r = tcp_recv(s->ctl, (uint8_t *)s->lb + s->ln,
                     (uint16_t)(sizeof(s->lb) - 1 - s->ln));
    if (r > 0)
        s->ln += (uint16_t)r;
    for (uint16_t i = 0; s->used && i < s->ln; i++) {
        if (s->lb[i] != '\n')
            continue;
        char line[512];
        uint16_t n = i && s->lb[i - 1] == '\r' ? i - 1 : i;
        memcpy(line, s->lb, n);
        line[n] = 0;
        memmove(s->lb, s->lb + i + 1, s->ln - i - 1);
        s->ln -= i + 1;
        i = (uint16_t)-1; /* (from the start again) */
        command(s, line);
    }
    if (s->used && s->ln >= sizeof(s->lb) - 1) {
        s->ln = 0;
        say(s, "500 Too long.");
    }
}

void ftpd_poll(void) {
    if (!s_running)
        return;
    net_poll();
    for (int c = tcp_accept(s_listen); c >= 0; c = tcp_accept(s_listen)) {
        sess_t *s = NULL;
        for (int i = 0; i < FTPD_SESSIONS && !s; i++)
            if (!s_sess[i].used)
                s = &s_sess[i];
        if (!s) {
            const char *m = "421 Too many at once, later.\r\n";
            tcp_send(c, (const uint8_t *)m, (uint16_t)strlen(m));
            tcp_release(c);
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->ctl = c;
        s->pasv = s->data = -1;
        strcpy(s->cwd, "/");
        s->idle_t = time_millis();
        say(s, "220 ASMOS FTP server ready.");
    }
    for (int i = 0; i < FTPD_SESSIONS; i++) {
        sess_t *s = &s_sess[i];
        if (!s->used)
            continue;
        if (s->xfer != X_NONE)
            pump(s);
        if (s->used)
            session_input(s);
        tcp_state_t st = tcp_state(s->ctl);
        bool gone = st != TCP_ESTABLISHED;
        bool idle = s->xfer == X_NONE && time_millis() - s->idle_t > IDLE_MS;
        if (s->used && (gone || idle)) {
            if (idle)
                say(s, "421 Quiet too long, bye.");
            end_xfer(s, true);
            tcp_release(s->ctl);
            s->used = false;
        }
    }
}

static void ftpd_task(void *arg) {
    (void)arg;
    while (s_running) {
        ftpd_poll();
        task_yield();
    }
    s_task = -1;
}

bool ftpd_start(uint16_t port, bool read_only, const char *password,
                const char **err) {
    *err = NULL;
    if (s_running) {
        *err = "The server is running already (ftp stopsrv).";
        return false;
    }
    if (!net_ready()) {
        *err = "No network (or no address yet).";
        return false;
    }
    s_sess = kzalloc(FTPD_SESSIONS * sizeof(sess_t));
    if (!s_sess) {
        *err = "Not enough memory.";
        return false;
    }
    s_listen = tcp_listen(port);
    if (s_listen < 0) {
        kfree(s_sess);
        s_sess = NULL;
        *err = "That port is taken.";
        return false;
    }
    s_port = port;
    s_ro = read_only;
    snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    s_sent = s_got = 0;
    s_running = true;
    if (s_task < 0)
        s_task = scheduler_add_task(ftpd_task, NULL, NULL);
    return true;
}

void ftpd_stop(void) {
    if (!s_running)
        return;
    s_running = false;
    for (int i = 0; i < FTPD_SESSIONS; i++) {
        sess_t *s = &s_sess[i];
        if (!s->used)
            continue;
        if (s->xfer != X_NONE)
            say(s, "426 The server is stopping.");
        say(s, "421 The server is stopping.");
        end_xfer(s, true);
        tcp_release(s->ctl);
    }
    tcp_release(s_listen);
    s_listen = -1;
    kfree(s_sess);
    s_sess = NULL;
}
