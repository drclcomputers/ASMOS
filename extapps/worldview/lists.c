// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* Bookmarks, the history, the downloads: a line each in a text file in
 * /PREFS, its fields split by tabs, and pages of World View's own that
 * show them (about:bookmarks, about:history, about:downloads), with
 * buttons. A button is a form sent to the same page (about:downloads?n=3
 * &do=Open): done, and the page shown again at its plain address, so Back
 * and Reload don't do it twice.
 *
 *   WVBOOK.TXT  url, title                       (in the order added)
 *   WVHIST.TXT  "2026-11-27 19:05", url, title    (oldest first)
 *   WVDOWN.TXT  when, D/F/S, size, path, url, why (oldest first) */

#include "extapps/worldview/wv.h"

#define LIST_MAX (128u * 1024u) /* a list file read, at most */
#define VISITS_MAX                                                             \
    (96u * 1024u) /* the history: past that, the older half                    \
                     goes */
#define LINES_MAX 600

// the files

typedef struct {
    char *buf;
    int n;
    char *line[LINES_MAX];
} lines_t;

/* the file's lines (their tabs: fields); false: none */
static bool lines_read(const char *path, lines_t *l) {
    l->buf = NULL;
    l->n = 0;
    app_dirent_t de;
    if (!A->file_info(path, &de) || !de.size)
        return false;
    uint32_t want = de.size < LIST_MAX ? de.size : LIST_MAX;
    uint32_t skip = de.size - want; /* (too long: its end) */
    l->buf = (char *)kmalloc(want + 1);
    if (!l->buf)
        return false;
    int got = A->file_read_at(path, skip, l->buf, (int)want);
    if (got <= 0) {
        kfree(l->buf);
        l->buf = NULL;
        return false;
    }
    l->buf[got] = 0;
    char *p = l->buf;
    if (skip) { /* (a line cut at the start: not one) */
        char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + got;
    }
    while (*p && l->n < LINES_MAX) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        if (*p)
            l->line[l->n++] = p;
        if (!nl)
            break;
        p = nl + 1;
    }
    return l->n > 0;
}

static void lines_free(lines_t *l) {
    kfree(l->buf);
    l->buf = NULL;
    l->n = 0;
}

/* field k of a line (its tabs made ends: once, by fields()) */
static int fields(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++)
        if (*p == '\t') {
            *p = 0;
            f[n++] = p + 1;
        }
    for (int k = n; k < max; k++)
        f[k] = "";
    return n;
}

/* the lines written back, but line skip (-1: all of them) */
static bool lines_write(const char *path, lines_t *l, int skip) {
    buf_t b = {0};
    for (int i = 0; i < l->n; i++)
        if (i != skip) {
            bput(&b, l->line[i]);
            bput(&b, "\n");
        }
    if (b.cap && !b.p)
        return false;
    bool ok = A->file_write(path, b.p ? b.p : "", (int)b.n);
    kfree(b.p);
    return ok;
}

/* the /PREFS folder (made if it isn't there) */
static bool prefs_dir(void) {
    app_dirent_t de;
    return A->file_info("/PREFS", &de) || A->make_dir("/PREFS");
}

/* a field: its tabs and line ends made spaces */
static void put_field(char *out, int sz, const char *s) {
    int k = 0;
    for (; s && *s && k < sz - 1; s++)
        out[k++] = *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s;
    out[k] = 0;
}

static void now_text(char *out, int sz) {
    time_full_t t = time_rtc_local();
    snprintf(out, sz, "%04u-%02u-%02u %02u:%02u", (unsigned)t.year,
             (unsigned)t.month, (unsigned)t.day, (unsigned)t.hours,
             (unsigned)t.minutes);
}

static bool append(const char *path, const char *line) {
    return prefs_dir() && A->file_append(path, line, (int)strlen(line));
}

// keeping

bool list_bookmark(const char *url, const char *title) {
    lines_t l;
    if (lines_read(WV_BOOKMARKS, &l)) {
        for (int i = 0; i < l.n; i++) {
            char *f[2];
            fields(l.line[i], f, 2);
            if (!strcmp(f[0], url)) {
                lines_free(&l);
                return false;
            }
        }
        lines_free(&l);
    }
    char u[WV_URL], t[128], line[WV_URL + 140];
    put_field(u, sizeof(u), url);
    put_field(t, sizeof(t), title && title[0] ? title : url);
    snprintf(line, sizeof(line), "%s\t%s\n", u, t);
    return append(WV_BOOKMARKS, line);
}

void list_visit(const char *url, const char *title) {
    static char last[WV_URL]; /* (a reload, or the same page again) */
    if (!strncasecmp(url, "about:", 6) || !strcmp(url, last))
        return;
    strlcpy(last, url, sizeof(last));
    char when[20], u[WV_URL], t[128], line[WV_URL + 160];
    now_text(when, sizeof(when));
    put_field(u, sizeof(u), url);
    put_field(t, sizeof(t), title);
    snprintf(line, sizeof(line), "%s\t%s\t%s\n", when, u, t);
    append(WV_VISITS, line);
    app_dirent_t de;
    if (A->file_info(WV_VISITS, &de) && de.size > VISITS_MAX) {
        lines_t l; /* (too long: the older half goes) */
        if (lines_read(WV_VISITS, &l)) {
            int half = l.n / 2;
            for (int i = 0; i + half < l.n; i++)
                l.line[i] = l.line[i + half];
            l.n -= half;
            lines_write(WV_VISITS, &l, -1);
            lines_free(&l);
        }
    }
}

void list_download(char state, const char *path, const char *url, uint32_t size,
                   const char *why) {
    char when[20], p[100], u[WV_URL], w[100], line[WV_URL + 240];
    now_text(when, sizeof(when));
    put_field(p, sizeof(p), path);
    put_field(u, sizeof(u), url);
    put_field(w, sizeof(w), why);
    snprintf(line, sizeof(line), "%s\t%c\t%u\t%s\t%s\t%s\n", when, state,
             (unsigned)size, p, u, w);
    append(WV_DOWNLOADS, line);
}

// the pages

/* "n=3&do=Show+in+FileF": the values */
static void query(const char *q, int *n, char *act, int asz) {
    *n = -1;
    act[0] = 0;
    while (q && *q) {
        const char *e = strchr(q, '&');
        int len = e ? (int)(e - q) : (int)strlen(q);
        char kv[80];
        snprintf(kv, sizeof(kv), "%.*s", len < 79 ? len : 79, q);
        url_decode(kv, true);
        if (!strncmp(kv, "n=", 2))
            *n = (int)str_to_uint32(kv + 2);
        else if (!strncmp(kv, "do=", 3))
            strlcpy(act, kv + 3, (size_t)asz);
        q = e ? e + 1 : NULL;
    }
}

/* a form of buttons for item n of a page */
static void form_open(buf_t *b, const char *page, int n) {
    char h[96];
    snprintf(h, sizeof(h),
             "<form action=\"about:%s\"><input type=\"hidden\" name=\"n\" "
             "value=\"%d\">",
             page, n);
    bput(b, h);
}

static void button(buf_t *b, const char *label) {
    bput(b, "<input type=\"submit\" name=\"do\" value=\"");
    bput_text(b, label, 40);
    bput(b, "\"> ");
}

static void put_link(buf_t *b, const char *url, const char *label) {
    bput(b, "<a href=\"");
    bput_text(b, url, WV_URL);
    bput(b, "\">");
    bput_text(b, label && label[0] ? label : url, 200);
    bput(b, "</a>");
}

static void bookmarks_page(buf_t *b, int n, const char *act) {
    lines_t l;
    bool any = lines_read(WV_BOOKMARKS, &l);
    if (any && !strcmp(act, "Remove") && n >= 0 && n < l.n) {
        lines_write(WV_BOOKMARKS, &l, n);
        lines_free(&l);
        any = lines_read(WV_BOOKMARKS, &l);
    }
    bput(b, "<title>Bookmarks</title><h2>Bookmarks</h2>");
    if (!any) {
        bput(b, "<p>No bookmarks yet. On a page you want to come back to: "
                "Bookmarks &gt; Add Bookmark (Ctrl+D).");
        return;
    }
    for (int i = 0; i < l.n; i++) {
        char *f[2];
        fields(l.line[i], f, 2);
        form_open(b, "bookmarks", i);
        put_link(b, f[0], f[1]);
        bput(b, " ");
        button(b, "Remove");
        bput(b, "<br><small>");
        bput_text(b, f[0], WV_URL);
        bput(b, "</small></form>");
    }
    lines_free(&l);
}

/* "2026-11-27": Today, Yesterday, or "27 November 2026" */
static void day_name(const char *when, char *out, int sz) {
    int y = (int)str_to_uint32(when), m = (int)str_to_uint32(when + 5),
        d = (int)str_to_uint32(when + 8);
    time_full_t t = time_rtc_local();
    int32_t days = A->time->days_from_civil(y, m, d),
            today = A->time->days_from_civil((int)t.year, t.month, t.day);
    if (days == today)
        strlcpy(out, "Today", (size_t)sz);
    else if (days == today - 1)
        strlcpy(out, "Yesterday", (size_t)sz);
    else if (m >= 1 && m <= 12)
        snprintf(out, sz, "%d %s %d", d, A->time->month_name(m), y);
    else
        strlcpy(out, "Earlier", (size_t)sz);
}

static void history_page(buf_t *b, const char *act) {
    if (!strcmp(act, "Clear History"))
        A->remove(WV_VISITS);
    bput(b, "<title>History</title><h2>History</h2>");
    lines_t l;
    if (!lines_read(WV_VISITS, &l)) {
        bput(b, "<p>Nothing yet: the pages you visit are listed here.");
        return;
    }
    char *(*f)[3] = kmalloc(sizeof(*f) * (uint32_t)l.n);
    if (!f) {
        lines_free(&l);
        return;
    }
    for (int i = 0; i < l.n; i++)
        fields(l.line[i], f[i], 3);
    bput(b, "<form action=\"about:history\">");
    button(b, "Clear History");
    bput(b, "</form>");
    char day[40] = "";
    int shown = 0;
    for (int i = l.n - 1; i >= 0 && shown < 300; i--) { /* newest first */
        bool again = !f[i][1][0]; /* (seen later: shown there) */
        for (int k = l.n - 1; k > i && !again; k--)
            again = !strcmp(f[k][1], f[i][1]);
        if (again)
            continue;
        char name[40];
        day_name(f[i][0], name, sizeof(name));
        if (strcmp(name, day)) {
            strlcpy(day, name, sizeof(day));
            bput(b, "<h3>");
            bput_text(b, day, 40);
            bput(b, "</h3>");
        }
        bput(b, "<tt>");
        bput_text(b, strlen(f[i][0]) > 11 ? f[i][0] + 11 : "", 5);
        bput(b, "</tt> ");
        put_link(b, f[i][1], f[i][2]);
        bput(b, "<br>");
        shown++;
    }
    kfree(f);
    lines_free(&l);
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void downloads_page(buf_t *b, int n, const char *act) {
    lines_t l;
    bool any = lines_read(WV_DOWNLOADS, &l);
    if (!strcmp(act, "Stop")) {
        wv_download_stop();
        lines_free(&l);
        any = lines_read(WV_DOWNLOADS, &l);
    } else if (!strcmp(act, "Clear List")) {
        A->remove(WV_DOWNLOADS);
        lines_free(&l);
        any = false;
    } else if (any && n >= 0 && n < l.n) {
        char copy[WV_URL + 240], *f[6];
        strlcpy(copy, l.line[n], sizeof(copy));
        fields(copy, f, 6);
        if (!strcmp(act, "Open"))
            A->open_path(f[3]);
        else if (!strcmp(act, "Show in FileF"))
            A->reveal_path(f[3]);
        else if (!strcmp(act, "Remove from List")) {
            lines_write(WV_DOWNLOADS, &l, n);
            lines_free(&l);
            any = lines_read(WV_DOWNLOADS, &l);
        }
    }
    bput(b, "<title>Downloads</title><h2>Downloads</h2><p>Files go into "
            "the DOWNLOAD folder. Save Link As (right-click a link) asks "
            "where.");
    const char *name = NULL;
    const wv_fetch_t *dl = wv_download(&name);
    if (!any && !dl) {
        bput(b, "<p>Nothing downloaded yet.");
        return;
    }
    bput(b, "<form action=\"about:downloads\">");
    button(b, "Clear List");
    bput(b, "</form>");
    if (dl) {
        char t[120];
        form_open(b, "downloads", -1);
        bput(b, "<b>");
        bput_text(b, name, 64);
        bput(b, "</b><br>");
        if (dl->clen > 0)
            snprintf(t, sizeof(t), "Downloading: %u of %u KB ",
                     (unsigned)(dl->total / 1024),
                     (unsigned)((uint32_t)dl->clen / 1024));
        else
            snprintf(t, sizeof(t), "Downloading: %u KB ",
                     (unsigned)(dl->total / 1024));
        bput(b, t);
        button(b, "Stop");
        bput(b, "</form>");
    }
    for (int i = l.n - 1; i >= 0; i--) { /* newest first */
        char *f[6];
        fields(l.line[i], f, 6);
        const char *path = f[3], *url = f[4];
        char t[160];
        form_open(b, "downloads", i);
        bput(b, "<b>");
        bput_text(b, base_name(path), 64);
        bput(b, "</b><br>");
        unsigned size_kb = (unsigned)((str_to_uint32(f[2]) + 1023) / 1024);
        snprintf(t, sizeof(t), "%s, %u KB. ", f[0], size_kb);
        bput_text(b, t, 160);
        app_dirent_t de;
        if (f[1][0] == 'D' && A->file_info(path, &de)) {
            bput(b, "In <tt>");
            bput_text(b, path, 100);
            bput(b, "</tt><br>");
            button(b, "Open");
            button(b, "Show in FileF");
        } else if (f[1][0] == 'D') {
            bput(b, "<i>Moved or deleted.</i><br>");
        } else {
            bput(b, f[1][0] == 'F' ? "<i>Failed" : "<i>Stopped");
            if (f[5][0]) {
                bput(b, ": ");
                bput_text(b, f[5], 100);
            }
            bput(b, ".</i> ");
            put_link(b, url, "Try again");
            bput(b, "<br>");
        }
        button(b, "Remove from List");
        bput(b, "<br><small>From ");
        bput_text(b, url, WV_URL);
        bput(b, "</small></form>");
    }
    lines_free(&l);
}

char *list_page(const char *url, uint32_t *len, char *show_at, int sz) {
    char page[32];
    const char *p = url + 6; /* (after "about:") */
    int k = 0;
    while (p[k] && p[k] != '?' && k < (int)sizeof(page) - 1) {
        page[k] = (char)(p[k] >= 'A' && p[k] <= 'Z' ? p[k] + 32 : p[k]);
        k++;
    }
    page[k] = 0;
    const char *q = p[k] == '?' ? p + k + 1 : NULL;
    int n;
    char act[40];
    query(q, &n, act, sizeof(act));
    buf_t b = {0};
    if (!strcmp(page, "bookmarks"))
        bookmarks_page(&b, n, act);
    else if (!strcmp(page, "history"))
        history_page(&b, act);
    else if (!strcmp(page, "downloads"))
        downloads_page(&b, n, act);
    else
        return NULL;
    if (!b.p)
        return NULL;
    snprintf(show_at, sz, "about:%s", page);
    *len = b.n;
    return b.p;
}
