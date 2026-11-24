// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "fs/netfs.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "network/dns.h"
#include "network/ftp.h"
#include "network/gopher.h" /* parse_ip_str */
#include "network/net.h"
#include "os/scheduler.h"

#define NCONN 3    /* commands, and a transfer or two at once */
#define NDIRS 1024 /* folder handles: 10 bits of slot, 6 of generation */
#define SLOT_BITS 10
#define NLIST 8          /* listings kept */
#define LIST_TTL 3000    /* ms a listing is trusted */
#define MAX_ENTRIES 2000 /* (a folder with more: the rest isn't shown) */
#define SMALL 32768      /* files up to this are read whole at once */
#define NFILES 6
#define NAME_MAX_LEN 120

typedef struct {
    char name[NAME_MAX_LEN + 1]; /* as on the server */
    char alias[11];
    uint8_t attr;
    uint32_t size;
    uint16_t date, time;
    uint16_t handle; /* a folder's, once given */
} nent_t;

typedef struct {
    int n, cap;
    nent_t *e;
} listing_t;

typedef struct {
    char *path; /* NULL: free */
    uint8_t gen;
    uint32_t used_t;
    listing_t *ls;
    uint32_t ls_t;
} ndir_t;

typedef struct {
    bool used;
    char *path;
    uint16_t dir;
    ftp_t *c; /* the transfer going on, or NULL */
    bool writing;
    uint32_t pos;   /* where the transfer is */
    uint8_t *whole; /* a small file, read at once */
} nfile_t;

static ftp_t s_conn[NCONN];
static bool s_up;
static uint8_t s_ip[4];
static uint16_t s_port;
static char s_host[64], s_user[32], s_pass[32];
static bool s_mlsd;
static ndir_t *s_dirs; /* [NDIRS]; slot 0 = the top */
static nfile_t s_files[NFILES];
static char s_err[128];
static int s_owner = -1, s_depth; /* the task inside, and how deep */

const char *netfs_last_error(void) { return s_err; }
bool netfs_connected(void) { return s_up; }

static void fail(const char *why) { snprintf(s_err, sizeof(s_err), "%s", why); }

static void fail_ftp(const ftp_t *c, const char *what) {
    if (c && c->code)
        snprintf(s_err, sizeof(s_err), "%s: %d %s", what, c->code, c->msg);
    else
        snprintf(s_err, sizeof(s_err), "%s: %s", what,
                 c ? c->msg : "not connected");
}

// one task at a time (another waits its turn)

static bool enter(void) {
    if (!s_up)
        return false;
    uint32_t t0 = time_millis();
    while (s_depth && s_owner != current_task) {
        if (time_millis() - t0 > 60000)
            return false;
        task_yield();
    }
    s_owner = current_task;
    s_depth++;
    return s_up;
}

static void leave(void) {
    if (s_depth && --s_depth == 0)
        s_owner = -1;
}

// connections

static bool reopen(ftp_t *c) {
    if (ftp_alive(c))
        return true;
    ftp_close(c);
    if (!ftp_open(c, s_ip, s_port, s_user, s_pass)) {
        fail_ftp(c, "Can't log in again");
        return false;
    }
    return true;
}

/* one free for a command or a transfer: logged in again if it was dropped
 * (servers let quiet ones go) */
static ftp_t *conn(void) {
    for (int i = 0; i < NCONN; i++)
        if (!s_conn[i].busy && ftp_alive(&s_conn[i]))
            return &s_conn[i];
    for (int i = 0; i < NCONN; i++)
        if (!s_conn[i].busy && reopen(&s_conn[i]))
            return &s_conn[i];
    if (!s_err[0])
        fail("All connections are busy.");
    return NULL;
}

/* a command; once more on a new connection if the old one was dropped */
static int cmd(const char *fmt, const char *arg, ftp_t **used) {
    char b[FS_PATH_SZ + 16];
    snprintf(b, sizeof(b), fmt, arg);
    for (int tries = 0; tries < 2; tries++) {
        ftp_t *c = conn();
        if (!c)
            return 0;
        int k = ftp_cmd(c, b);
        if (used)
            *used = c;
        if (k)
            return k;
    }
    return 0;
}

static bool xfer(ftp_t **out, const char *fmt, const char *path,
                 uint32_t rest) {
    char b[FS_PATH_SZ + 16];
    snprintf(b, sizeof(b), fmt, path);
    for (int tries = 0; tries < 2; tries++) {
        ftp_t *c = conn();
        if (!c)
            return false;
        if (ftp_xfer_begin(c, b, rest)) {
            *out = c;
            return true;
        }
        if (c->code) { /* the server said no (550 ...) */
            fail_ftp(c, "The server says");
            return false;
        }
    }
    fail("The connection was lost.");
    return false;
}

// folders: handles and paths

static void join(char *out, const char *dir, const char *name) {
    if (!strcmp(dir, "/"))
        snprintf(out, FS_PATH_SZ, "/%s", name);
    else
        snprintf(out, FS_PATH_SZ, "%s/%s", dir, name);
}

static const char *dir_path(uint16_t h) {
    if (!s_dirs)
        return NULL;
    int slot = h & (NDIRS - 1);
    ndir_t *d = &s_dirs[slot];
    if (!d->path || (slot && d->gen != (h >> SLOT_BITS)) || (!slot && h))
        return NULL;
    d->used_t = time_millis();
    return d->path;
}

static uint16_t handle_of(int slot) {
    return (uint16_t)(slot | (s_dirs[slot].gen << SLOT_BITS));
}

static void drop_listing(ndir_t *d) {
    if (d->ls) {
        kfree(d->ls->e);
        kfree(d->ls);
        d->ls = NULL;
    }
}

/* a handle for a folder (the same one again for the same path); 0xFFFF:
 * no memory */
static uint16_t new_handle(const char *path) {
    int free_slot = -1, old = -1;
    for (int i = 1; i < NDIRS; i++) {
        ndir_t *d = &s_dirs[i];
        if (!d->path) {
            if (free_slot < 0)
                free_slot = i;
        } else if (!strcmp(d->path, path)) {
            return handle_of(i);
        } else if (old < 0 || d->used_t < s_dirs[old].used_t) {
            old = i;
        }
    }
    int i = free_slot >= 0 ? free_slot : old;
    char *p = kmalloc(strlen(path) + 1);
    if (!p)
        return 0xFFFF;
    strcpy(p, path);
    ndir_t *d = &s_dirs[i];
    kfree(d->path);
    drop_listing(d);
    d->path = p;
    d->gen = (uint8_t)((d->gen + 1) & 63); /* (old handles to it go stale) */
    d->used_t = time_millis();
    return handle_of(i);
}

/* a folder changed: its listing is fetched again */
static void stale(uint16_t h) {
    if (dir_path(h)) /* (as if fetched long ago) */
        s_dirs[h & (NDIRS - 1)].ls_t = time_millis() - LIST_TTL - 1;
}

// listings

static const char *const k_mon[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};

static void fat_stamp(nent_t *e, int y, int mo, int d, int h, int mi, int s) {
    if (y < 1980 || y > 2107 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        e->date = (uint16_t)((1 << 5) | 1); /* 1980-01-01 */
        e->time = 0;
        return;
    }
    e->date = (uint16_t)((y - 1980) << 9 | mo << 5 | d);
    e->time = (uint16_t)(h << 11 | mi << 5 | s / 2);
}

static int num(const char *p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

static bool set_name(nent_t *e, const char *n, int len) {
    while (len && (n[len - 1] == '\r' || n[len - 1] == ' '))
        len--;
    if (len <= 0 || len > NAME_MAX_LEN || (len == 1 && n[0] == '.') ||
        (len == 2 && n[0] == '.' && n[1] == '.'))
        return false;
    memcpy(e->name, n, len);
    e->name[len] = 0;
    return true;
}

/* "type=file;size=1234;modify=20240131120000; name" */
static bool parse_mlsd(const char *l, nent_t *e) {
    const char *sp = strchr(l, ' ');
    if (!sp)
        return false;
    bool file = false, dir = false;
    for (const char *f = l; f < sp;) {
        const char *end = f;
        while (end < sp && *end != ';')
            end++;
        if (!strncasecmp(f, "type=", 5)) {
            file = !strncasecmp(f + 5, "file", 4);
            dir = !strncasecmp(f + 5, "dir", 3);
        } else if (!strncasecmp(f, "size=", 5)) {
            uint32_t v = 0;
            for (const char *p = f + 5; p < end && *p >= '0' && *p <= '9'; p++)
                v = v * 10 + (uint32_t)(*p - '0');
            e->size = v;
        } else if (!strncasecmp(f, "modify=", 7) && end - f >= 21) {
            const char *p = f + 7;
            fat_stamp(e, num(p, 4), num(p + 4, 2), num(p + 6, 2), num(p + 8, 2),
                      num(p + 10, 2), num(p + 12, 2));
        }
        f = end < sp ? end + 1 : end;
    }
    if (!file && !dir) /* (cdir, pdir, links) */
        return false;
    e->attr = dir ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    return set_name(e, sp + 1, (int)strlen(sp + 1));
}

/* the words of a line: where each starts */
static int words(const char *l, const char *w[], int max) {
    int n = 0;
    const char *p = l;
    while (*p && n < max) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        w[n++] = p;
        while (*p && *p != ' ')
            p++;
    }
    return n;
}

static uint32_t to_u32(const char *p) {
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (uint32_t)(*p++ - '0');
    return v;
}

/* "drwxr-xr-x 2 user group 4096 Jan 31 12:00 name" (the group may be
 * missing), or DOS/IIS: "01-31-24  12:00PM  <DIR>  name" */
static bool parse_list(const char *l, nent_t *e) {
    const char *w[12];
    int n = words(l, w, 12);
    if (n < 4)
        return false;
    if (l[0] >= '0' && l[0] <= '9') { /* DOS */
        int mo = num(w[0], 2), d = num(w[0] + 3, 2), y = num(w[0] + 6, 2);
        int h = num(w[1], 2), mi = num(w[1] + 3, 2);
        if (mo < 0 || h < 0)
            return false;
        if ((w[1][5] == 'P' || w[1][5] == 'p') && h < 12)
            h += 12;
        if ((w[1][5] == 'A' || w[1][5] == 'a') && h == 12)
            h = 0;
        if (y >= 0)
            y += y < 70 ? 2000 : 1900;
        else
            y = num(w[0] + 6, 4);
        fat_stamp(e, y, mo, d, h, mi, 0);
        bool dir = !strncmp(w[2], "<DIR>", 5);
        e->attr = dir ? ATTR_DIRECTORY : ATTR_ARCHIVE;
        e->size = dir ? 0 : to_u32(w[2]);
        return set_name(e, w[3], (int)strlen(w[3]));
    }
    char kind = l[0];
    if (kind != '-' && kind != 'd' && kind != 'l')
        return false;
    /* the month: the size before it, the day and the time or year after */
    for (int m = 2; m + 3 < n; m++) {
        int mo = -1;
        for (int k = 0; k < 12; k++)
            if (!strncasecmp(w[m], k_mon[k], 3) && w[m][3] == ' ')
                mo = k + 1;
        if (mo < 0)
            continue;
        int d = (int)to_u32(w[m + 1]);
        const char *t = w[m + 2];
        int y, h = 0, mi = 0;
        time_full_t now = time_rtc_local();
        if (t[1] == ':' || t[2] == ':') { /* this year (or the last) */
            h = (int)to_u32(t);
            mi = (int)to_u32(strchr(t, ':') + 1);
            y = (int)now.year;
            if (mo > now.month + 1)
                y--;
        } else {
            y = (int)to_u32(t);
        }
        fat_stamp(e, y, mo, d, h, mi, 0);
        e->size = to_u32(w[m - 1]);
        e->attr = kind == '-' ? ATTR_ARCHIVE : ATTR_DIRECTORY;
        const char *name = w[m + 3];
        int len = (int)strlen(name);
        const char *arrow = kind == 'l' ? strstr(name, " -> ") : NULL;
        if (arrow) /* a link: taken as a folder (they mostly are) */
            len = (int)(arrow - name);
        if (kind != '-')
            e->size = 0;
        return set_name(e, name, len);
    }
    return false;
}

static bool alias_taken(const listing_t *ls, int upto, const char *a) {
    for (int i = 0; i < upto; i++)
        if (ls->e[i].alias[0] && !memcmp(ls->e[i].alias, a, 11))
            return true;
    return false;
}

/* the name as 8.3 if it is one, else NAME~N.EXT (unique here) */
static void give_aliases(listing_t *ls) {
    for (int i = 0; i < ls->n; i++) { /* names that fit first */
        nent_t *e = &ls->e[i];
        e->alias[0] = 0;
        bool ok = fs_name_is_83(e->name);
        for (const char *p = e->name; ok && *p; p++)
            ok = *p == '.' || fs_name_char_ok((char)toupper(*p));
        char a[12];
        if (ok) {
            fs_make_83(e->name, a);
            if (!alias_taken(ls, ls->n, a))
                memcpy(e->alias, a, 11);
        }
    }
    for (int i = 0; i < ls->n; i++) {
        nent_t *e = &ls->e[i];
        if (e->alias[0])
            continue;
        char base[8], ext[3];
        int bl = 0, el = 0;
        const char *dot = strrchr(e->name, '.');
        if (dot == e->name)
            dot = NULL;
        for (const char *p = e->name; *p && p != dot && bl < 6; p++) {
            char c = (char)toupper(*p);
            if (c != ' ' && c != '.')
                base[bl++] = fs_name_char_ok(c) ? c : '_';
        }
        for (const char *p = dot ? dot + 1 : ""; *p && el < 3; p++) {
            char c = (char)toupper(*p);
            if (c != ' ' && c != '.')
                ext[el++] = fs_name_char_ok(c) ? c : '_';
        }
        if (!bl)
            base[bl++] = '_';
        for (int k = 1; k < 100000; k++) {
            char tail[8], a[12];
            int tl = snprintf(tail, sizeof(tail), "~%d", k);
            int keep = bl < 8 - tl ? bl : 8 - tl;
            memset(a, ' ', 11);
            memcpy(a, base, keep);
            memcpy(a + keep, tail, tl);
            memcpy(a + 8, ext, el);
            if (!alias_taken(ls, ls->n, a)) {
                memcpy(e->alias, a, 11);
                break;
            }
        }
    }
}

static bool add_entry(listing_t *ls, const nent_t *e) {
    if (ls->n >= MAX_ENTRIES)
        return true;
    if (ls->n == ls->cap) {
        int cap = ls->cap ? ls->cap * 2 : 32;
        nent_t *m = kmalloc((uint32_t)cap * sizeof(nent_t));
        if (!m)
            return false;
        memcpy(m, ls->e, (uint32_t)ls->n * sizeof(nent_t));
        kfree(ls->e);
        ls->e = m;
        ls->cap = cap;
    }
    ls->e[ls->n++] = *e;
    return true;
}

/* only so many listings kept: the oldest goes */
static void trim_listings(void) {
    int n = 0, old = -1;
    for (int i = 0; i < NDIRS; i++)
        if (s_dirs[i].ls) {
            n++;
            if (old < 0 || s_dirs[i].used_t < s_dirs[old].used_t)
                old = i;
        }
    if (n > NLIST)
        drop_listing(&s_dirs[old]);
}

/* the folder's listing: kept, or fetched (MLSD, or LIST) */
static listing_t *get_listing(uint16_t h) {
    const char *path = dir_path(h);
    if (!path) {
        fail("That folder is gone (the drive was reconnected?).");
        return NULL;
    }
    ndir_t *d = &s_dirs[h & (NDIRS - 1)];
    if (d->ls && time_millis() - d->ls_t < LIST_TTL)
        return d->ls;
    listing_t *ls = kzalloc(sizeof(listing_t));
    ftp_t *c;
    if (!ls || !xfer(&c, s_mlsd ? "MLSD %s" : "LIST %s", path, 0)) {
        kfree(ls);
        return NULL;
    }
    static char line[600];
    static uint8_t chunk[1024];
    int ln = 0, r;
    bool ok = true;
    while ((r = ftp_xfer_read(c, chunk, sizeof(chunk))) > 0 && ok) {
        for (int i = 0; i < r && ok; i++) {
            if (chunk[i] != '\n') {
                if (ln < (int)sizeof(line) - 1)
                    line[ln++] = (char)chunk[i];
                continue;
            }
            line[ln] = 0;
            ln = 0;
            nent_t e;
            memset(&e, 0, sizeof(e));
            if (s_mlsd ? parse_mlsd(line, &e) : parse_list(line, &e))
                ok = add_entry(ls, &e);
        }
    }
    if (!ftp_xfer_end(c, r < 0) || r < 0 || !ok) {
        fail_ftp(c, "Can't list the folder");
        kfree(ls->e);
        kfree(ls);
        return NULL;
    }
    give_aliases(ls);
    /* the folders keep their handles (windows show them) */
    for (int i = 0; i < ls->n; i++) {
        if (!(ls->e[i].attr & ATTR_DIRECTORY) || !d->ls)
            continue;
        for (int k = 0; k < d->ls->n; k++)
            if (!strcmp(d->ls->e[k].name, ls->e[i].name)) {
                ls->e[i].handle = d->ls->e[k].handle;
                break;
            }
    }
    drop_listing(d);
    d->ls = ls;
    d->ls_t = time_millis();
    trim_listings();
    return ls;
}

/* the entry name83 in the folder */
static nent_t *find83(uint16_t dir, const char *name83) {
    listing_t *ls = get_listing(dir);
    for (int i = 0; ls && i < ls->n; i++)
        if (!memcmp(ls->e[i].alias, name83, 11))
            return &ls->e[i];
    if (ls)
        fail("No such file on the server.");
    return NULL;
}

static uint16_t child_handle(uint16_t dir, nent_t *e) {
    if (e->handle && dir_path(e->handle))
        return e->handle;
    char p[FS_PATH_SZ];
    join(p, dir_path(dir), e->name);
    e->handle = new_handle(p);
    return e->handle;
}

bool netfs_iterate(uint16_t dir, netfs_cb cb, void *user) {
    if (!enter())
        return false;
    listing_t *ls = get_listing(dir);
    bool ok = ls != NULL;
    for (int i = 0; ls && i < ls->n; i++) {
        nent_t *e = &ls->e[i];
        dir_entry_t de;
        memset(&de, 0, sizeof(de));
        memcpy(de.name, e->alias, 11);
        de.attr = e->attr;
        de.file_size = e->size;
        de.modify_date = de.create_date = e->date;
        de.modify_time = de.create_time = e->time;
        if (e->attr & ATTR_DIRECTORY) {
            uint16_t ch = child_handle(dir, e);
            if (ch == 0xFFFF)
                continue;
            de.cluster_lo = ch;
        }
        bool show = strlen(e->name) <= FS_NAME_MAX;
        for (const char *p = e->name; show && *p; p++)
            show = fs_long_char_ok(*p);
        /* (cb may fetch listings: this one may go; copy the name) */
        char nm[FS_NAME_SZ];
        if (show)
            strcpy(nm, e->name);
        if (!cb(&de, show ? nm : NULL, user))
            break;
        ls = s_dirs[dir & (NDIRS - 1)].ls; /* (still there?) */
    }
    leave();
    return ok;
}

// files

static nfile_t *file_of(const fat_file_t *f) {
    int i = f->dir_entry_idx;
    if (i < 0 || i >= NFILES || !s_files[i].used)
        return NULL;
    return &s_files[i];
}

static nfile_t *new_file(fat_file_t *f, uint16_t dir, const char *path) {
    for (int i = 0; i < NFILES; i++) {
        nfile_t *n = &s_files[i];
        if (n->used)
            continue;
        memset(n, 0, sizeof(*n));
        n->path = kmalloc(strlen(path) + 1);
        if (!n->path)
            break;
        strcpy(n->path, path);
        n->used = true;
        n->dir = dir;
        memset(f, 0, sizeof(*f));
        f->open = true;
        f->drive_id = DRIVE_NET0;
        f->dir_cluster = dir;
        f->dir_entry_idx = i;
        return n;
    }
    fail("Too many files open on NET0.");
    return NULL;
}

/* the transfer stops (cut short if it didn't end); false: an upload
 * failed */
static bool stop(nfile_t *n, bool cut) {
    if (!n->c)
        return true;
    bool ok = ftp_xfer_end(n->c, cut);
    if (!ok && n->writing)
        fail_ftp(n->c, "The upload failed");
    n->c = NULL;
    return ok;
}

bool netfs_open(fat_file_t *f, uint16_t dir, const char *name83) {
    if (!enter())
        return false;
    nent_t *e = find83(dir, name83);
    nfile_t *n = NULL;
    if (e && !(e->attr & ATTR_DIRECTORY)) {
        char p[FS_PATH_SZ];
        join(p, dir_path(dir), e->name);
        dir_entry_t de = {0};
        memcpy(de.name, e->alias, 11);
        de.attr = e->attr;
        de.file_size = e->size;
        de.modify_date = e->date;
        de.modify_time = e->time;
        n = new_file(f, dir, p);
        if (n)
            f->entry = de;
    }
    leave();
    return n != NULL;
}

bool netfs_create(fat_file_t *f, uint16_t dir, const char *name) {
    if (!enter())
        return false;
    const char *dp = dir_path(dir);
    char p[FS_PATH_SZ];
    nfile_t *n = NULL;
    if (dp) {
        join(p, dp, name);
        n = new_file(f, dir, p);
    }
    if (n && !xfer(&n->c, "STOR %s", p, 0)) { /* (it is there at once) */
        kfree(n->path);
        n->used = false;
        f->open = false;
        n = NULL;
    }
    if (n) {
        n->writing = true;
        f->entry.attr = ATTR_ARCHIVE;
        fs_make_83(name, (char *)f->entry.name); /* (writes a 0 into attr) */
        f->entry.attr = ATTR_ARCHIVE;
        stale(dir);
    }
    leave();
    return n != NULL;
}

/* a small file: all of it now */
static bool read_whole(fat_file_t *f, nfile_t *n) {
    uint32_t size = f->entry.file_size;
    n->whole = kmalloc(size ? size : 1);
    ftp_t *c;
    if (!n->whole || !xfer(&c, "RETR %s", n->path, 0)) {
        kfree(n->whole);
        n->whole = NULL;
        return false;
    }
    uint32_t got = 0;
    int r = 1;
    while (got < size &&
           (r = ftp_xfer_read(
                c, n->whole + got,
                (uint16_t)(size - got > 4096 ? 4096 : size - got))) > 0)
        got += (uint32_t)r;
    bool ok = ftp_xfer_end(c, got < size || r < 0) && got == size;
    if (!ok) {
        fail_ftp(c, "Can't read the file");
        kfree(n->whole);
        n->whole = NULL;
    }
    return ok;
}

int netfs_read(fat_file_t *f, void *buf, int len) {
    nfile_t *n = file_of(f);
    if (!n || !enter())
        return -1;
    uint32_t size = f->entry.file_size;
    int total = -1;
    if (n->writing) {
        fail("The file is open for writing.");
    } else if (f->cur_offset >= size) {
        total = 0;
    } else if (size <= SMALL && (n->whole || read_whole(f, n))) {
        uint32_t k = size - f->cur_offset;
        total = (int)(k < (uint32_t)len ? k : (uint32_t)len);
        memcpy(buf, n->whole + f->cur_offset, (uint32_t)total);
        f->cur_offset += (uint32_t)total;
    } else if (size > SMALL) {
        if (n->c && n->pos != f->cur_offset)
            stop(n, true); /* (it was moved: from there anew) */
        if (!n->c && xfer(&n->c, "RETR %s", n->path, f->cur_offset))
            n->pos = f->cur_offset;
        uint8_t *out = buf;
        total = n->c ? 0 : -1;
        while (n->c && total < len && n->pos < size) {
            int want = len - total;
            int r = ftp_xfer_read(n->c, out + total,
                                  (uint16_t)(want > 8192 ? 8192 : want));
            if (r <= 0) {
                stop(n, r < 0);
                break;
            }
            total += r;
            n->pos += (uint32_t)r;
        }
        if (n->c && n->pos >= size)
            stop(n, false); /* (all of it: the connection is free again) */
        if (total > 0)
            f->cur_offset += (uint32_t)total;
    }
    leave();
    return total;
}

int netfs_write(fat_file_t *f, const void *buf, int len) {
    nfile_t *n = file_of(f);
    if (!n || !enter())
        return -1;
    int done = -1;
    if (!n->c || !n->writing) { /* from the start (replace) or the end */
        stop(n, true);
        kfree(n->whole);
        n->whole = NULL;
        if (f->cur_offset == 0 || f->cur_offset == f->entry.file_size) {
            n->writing =
                xfer(&n->c, f->cur_offset ? "APPE %s" : "STOR %s", n->path, 0);
            n->pos = f->cur_offset;
            if (!f->cur_offset)
                f->entry.file_size = 0;
        } else {
            fail("Only from the start or at the end, over FTP.");
        }
    }
    if (n->c && n->writing && f->cur_offset == n->pos) {
        const uint8_t *in = buf;
        done = 0;
        while (done < len) {
            int k = len - done > 4096 ? 4096 : len - done;
            if (!ftp_xfer_write(n->c, in + done, (uint16_t)k)) {
                fail("The upload was cut off.");
                done = done ? done : -1;
                break;
            }
            done += k;
        }
        if (done > 0) {
            n->pos += (uint32_t)done;
            f->cur_offset = n->pos;
            if (n->pos > f->entry.file_size)
                f->entry.file_size = n->pos;
        }
    }
    leave();
    return done;
}

bool netfs_seek(fat_file_t *f, uint32_t off) {
    if (!file_of(f) || off > f->entry.file_size)
        return false;
    f->cur_offset = off; /* (the transfer follows when read) */
    return true;
}

bool netfs_close(fat_file_t *f) {
    nfile_t *n = file_of(f);
    f->open = false;
    if (!n)
        return false;
    bool ok = true;
    if (enter()) {
        bool w = n->writing;
        ok = stop(n, !w);
        if (w)
            stale(n->dir);
        leave();
    }
    kfree(n->whole);
    kfree(n->path);
    n->used = false;
    return ok;
}

// folders and names

bool netfs_mkdir(uint16_t dir, const char *name, uint16_t *out) {
    if (!enter())
        return false;
    const char *dp = dir_path(dir);
    char p[FS_PATH_SZ];
    bool ok = false;
    ftp_t *c = NULL;
    if (dp) {
        join(p, dp, name);
        ok = cmd("MKD %s", p, &c) == 257;
        if (!ok)
            fail_ftp(c, "Can't make the folder");
        stale(dir);
        if (ok && out)
            *out = new_handle(p);
    }
    leave();
    return ok;
}

bool netfs_delete(uint16_t dir, const char *name83) {
    if (!enter())
        return false;
    nent_t *e = find83(dir, name83);
    bool ok = false;
    ftp_t *c = NULL;
    if (e && !(e->attr & ATTR_DIRECTORY)) {
        char p[FS_PATH_SZ];
        join(p, dir_path(dir), e->name);
        ok = cmd("DELE %s", p, &c) / 100 == 2;
        if (!ok)
            fail_ftp(c, "Can't delete it");
        stale(dir);
    }
    leave();
    return ok;
}

/* a folder and (all) what's in it */
static bool remove_dir(const char *path, bool all, int depth) {
    ftp_t *c = NULL;
    if (all && depth < 16) {
        uint16_t h = new_handle(path);
        listing_t *ls = h == 0xFFFF ? NULL : get_listing(h);
        if (!ls)
            return false;
        /* (the names: the listing may be dropped on the way down) */
        int n = ls->n;
        char (*names)[NAME_MAX_LEN + 1] =
            kmalloc((uint32_t)(n ? n : 1) * (NAME_MAX_LEN + 1));
        uint8_t *dirs = kmalloc((uint32_t)(n ? n : 1));
        bool ok = names && dirs;
        for (int i = 0; ok && i < n; i++) {
            strcpy(names[i], ls->e[i].name);
            dirs[i] = (ls->e[i].attr & ATTR_DIRECTORY) != 0;
        }
        for (int i = 0; ok && i < n; i++) {
            char p[FS_PATH_SZ];
            join(p, path, names[i]);
            if (dirs[i])
                ok = remove_dir(p, true, depth + 1);
            else if (cmd("DELE %s", p, &c) / 100 != 2) {
                fail_ftp(c, "Can't delete it");
                ok = false;
            }
        }
        kfree(names);
        kfree(dirs);
        if (!ok)
            return false;
    }
    if (cmd("RMD %s", path, &c) / 100 != 2) {
        fail_ftp(c, "Can't remove the folder");
        return false;
    }
    return true;
}

bool netfs_rmdir(uint16_t dir, const char *name83, bool all) {
    if (!enter())
        return false;
    nent_t *e = find83(dir, name83);
    bool ok = false;
    if (e && (e->attr & ATTR_DIRECTORY)) {
        char p[FS_PATH_SZ];
        join(p, dir_path(dir), e->name);
        ok = remove_dir(p, all, 0);
        stale(dir);
    }
    leave();
    return ok;
}

bool netfs_rename(uint16_t dir, const char *name83, uint16_t to_dir,
                  const char *new_name) {
    if (!enter())
        return false;
    nent_t *e = find83(dir, name83);
    const char *td = dir_path(to_dir);
    bool ok = false;
    ftp_t *c = NULL;
    if (e && td) {
        char from[FS_PATH_SZ], to[FS_PATH_SZ];
        join(to, td, new_name);
        join(from, dir_path(dir), e->name);
        ok = cmd("RNFR %s", from, &c) == 350;
        if (ok) {
            char b[FS_PATH_SZ + 8];
            snprintf(b, sizeof(b), "RNTO %s", to);
            ok = ftp_cmd(c, b) / 100 == 2; /* (the same connection) */
        }
        if (!ok)
            fail_ftp(c, "Can't rename it");
        stale(dir);
        stale(to_dir);
    }
    leave();
    return ok;
}

// connecting

bool netfs_mount(fat_vol_t *v) {
    if (!s_up)
        return false;
    memset(v->label, 0, sizeof(v->label));
    strncpy(v->label, s_host, sizeof(v->label) - 1);
    v->mounted = true;
    return true;
}

void netfs_unmount(void) {}

/* 257 "/home/me" is the current directory: the path ("" doubles a ") */
static bool quoted(const char *m, char *out) {
    const char *p = strchr(m, '"');
    if (!p)
        return false;
    int o = 0;
    for (p++; *p && o < FS_PATH_MAX; p++) {
        if (*p == '"') {
            if (p[1] != '"')
                break;
            p++;
        }
        out[o++] = *p;
    }
    out[o] = 0;
    return o > 0;
}

static void feat_cb(const char *line, void *user) {
    if (strstr(line, "MLST") || strstr(line, "mlst"))
        *(bool *)user = true;
}

bool netfs_connect(const char *host, uint16_t port, const char *user,
                   const char *pass, const char *dir, const char **err) {
    s_err[0] = 0;
    *err = s_err;
    if (s_up)
        netfs_disconnect();
    if (!net_ready()) {
        fail("No network (or no address yet).");
        return false;
    }
    uint8_t ip[4];
    if (!parse_ip_str(host, ip) && !dns_resolve(host, ip)) {
        fail("Can't find that server (DNS).");
        return false;
    }
    s_dirs = kzalloc(NDIRS * sizeof(ndir_t));
    if (!s_dirs) {
        fail("Not enough memory.");
        return false;
    }
    memcpy(s_ip, ip, 4);
    s_port = port ? port : FTP_PORT;
    snprintf(s_host, sizeof(s_host), "%s", host);
    snprintf(s_user, sizeof(s_user), "%s", user && *user ? user : "anonymous");
    snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
    for (int i = 0; i < NCONN; i++)
        ftp_init(&s_conn[i]);
    ftp_t *c = &s_conn[0];
    char root[FS_PATH_SZ];
    bool ok = ftp_open(c, s_ip, s_port, s_user, s_pass);
    if (!ok)
        fail_ftp(c, c->code ? "The server says" : "Can't connect");
    if (ok && dir && *dir) {
        char b[FS_PATH_SZ + 8];
        snprintf(b, sizeof(b), "CWD %s", dir);
        ok = ftp_cmd(c, b) == 250;
        if (!ok)
            fail_ftp(c, "No such folder");
    }
    ok = ok && ftp_cmd(c, "PWD") == 257 && quoted(c->msg, root);
    if (!ok && !s_err[0])
        fail_ftp(c, "Can't tell where the folder is");
    s_mlsd = false;
    if (ok)
        ftp_cmd_lines(c, "FEAT", feat_cb, &s_mlsd);
    if (ok)
        ok = (s_dirs[0].path = kmalloc(strlen(root) + 1)) != NULL;
    if (!ok) {
        ftp_close(c);
        kfree(s_dirs);
        s_dirs = NULL;
        return false;
    }
    strcpy(s_dirs[0].path, root);
    s_up = true;
    s_depth = 0;
    s_owner = -1;
    if (!fs_mount_drive(DRIVE_NET0) || !get_listing(0)) {
        const char *why = s_err[0] ? s_err : "Can't list the folder.";
        char keep[128];
        snprintf(keep, sizeof(keep), "%s", why);
        netfs_disconnect();
        fail(keep);
        return false;
    }
    return true;
}

void netfs_disconnect(void) {
    if (!s_up)
        return;
    s_up = false;
    for (int i = 0; i < NFILES; i++)
        if (s_files[i].used) { /* (the windows reading them fail now) */
            if (s_files[i].c)
                ftp_xfer_end(s_files[i].c, true);
            kfree(s_files[i].whole);
            kfree(s_files[i].path);
            s_files[i].used = false;
        }
    for (int i = 0; i < NCONN; i++)
        ftp_close(&s_conn[i]);
    for (int i = 0; s_dirs && i < NDIRS; i++) {
        kfree(s_dirs[i].path);
        drop_listing(&s_dirs[i]);
    }
    kfree(s_dirs);
    s_dirs = NULL;
    fs_unmount(DRIVE_NET0);
}

void netfs_describe(char *out, int n) {
    if (!s_up) {
        snprintf(out, n, "not connected");
        return;
    }
    snprintf(out, n, "ftp://%s@%s:%u%s", s_user, s_host, s_port,
             s_dirs ? s_dirs[0].path : "");
}
