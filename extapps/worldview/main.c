// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* World View's window: Back, Forward, Reload, Stop, Home and the address
 * on top, the page under them with its scroll bar, what is going on in
 * the status line. A page is fetched, laid out, then its pictures are
 * fetched one after another, each put in when it comes. Links: the hand
 * over them, their address in the status line. Forms: click a field and
 * type; Enter (or its button) sends it. Options: the home page, a proxy
 * (for https:// and the modern web), pictures on or off. gopher:// and
 * ftp:// too: their menus and folders as pages. What isn't a page is a
 * download: it asks where, and goes on beside the pages. A right-click
 * on a link or a picture: open it, save it, copy its address. */

#include "extapps/worldview/wv.h"

#define DL_DIR "/DOWNLOAD" /* where downloads go */

app_descriptor worldview_app;
static wv_state_t *S; /* (the one running: menus, dialogs) */

static wv_state_t *me(void) {
    wv_state_t *s = ui_active_state();
    return s ? s : S;
}

// visited links (by a hash of the URL)

static uint32_t hash(const char *u) {
    uint32_t h = 2166136261u;
    while (*u)
        h = (h ^ (uint8_t)*u++) * 16777619u;
    return h | 1;
}

static bool visited(void *user, const char *url) {
    wv_state_t *s = user;
    uint32_t h = hash(url);
    for (int i = 0; i < s->nvisited && i < VISITED_N; i++)
        if (s->visited[i] == h)
            return true;
    return false;
}

static void visit(wv_state_t *s, const char *url) {
    if (!visited(s, url))
        s->visited[s->nvisited++ % VISITED_N] = hash(url);
}

// the status line, the scroll bar

static void say(wv_state_t *s, const char *t) {
    if (strcmp(s->line, t)) {
        strlcpy(s->line, t, sizeof(s->line));
        ctl_set_text(s->status, s->line);
    }
}

static int view_h(wv_state_t *s) { return s->view->ah - 4; }
static int view_w(wv_state_t *s) { return s->view->aw - 4; }

static void clamp_scroll(wv_state_t *s) {
    int32_t most = s->doc.height - view_h(s);
    most = most < 0 ? 0 : most;
    s->scroll = s->scroll > most ? most : s->scroll < 0 ? 0 : s->scroll;
    ctl_set_range(s->vs, 0, (int)most, view_h(s));
    ctl_set_value(s->vs, (int)s->scroll);
}

static void buttons(wv_state_t *s) {
    ctl_enable(s->btn[0], s->at > 0);
    ctl_enable(s->btn[1], s->at < s->nhist - 1);
    ctl_enable(s->btn[3], s->loading || s->pic_k >= 0 || s->dl_on);
}

static void set_title(wv_state_t *s) {
    const char *t = s->doc.title[0] ? s->doc.title : s->url;
    snprintf(s->title, sizeof(s->title), "%s - World View", t);
    s->win->title = s->title;
    wm_invalidate(s->win);
}

// laying the page out

static void layout(wv_state_t *s) {
    int w = view_w(s);
    doc_layout(&s->doc, w);
    s->laid_w = w;
    s->relayout = false;
    if (s->focus >= s->doc.nfields)
        s->focus = -1;
    clamp_scroll(s);
    wm_invalidate(s->win);
}

/* a web page can't reach files on this computer (a file's page can) */
static bool may_reach(const wv_state_t *s, const char *url) {
    if (!strncasecmp(url, "about:", 6)) /* (its buttons: its own pages) */
        return !strncasecmp(s->doc.base, "about:", 6);
    return strncasecmp(url, "file:", 5) == 0
               ? strncasecmp(s->doc.base, "file:", 5) == 0 ||
                     !strncasecmp(s->doc.base, "about:", 6)
               : true;
}

// pictures: one after another

static void next_picture(wv_state_t *s) {
    s->pic_k = -1;
    if (!s->images)
        return;
    for (int k = 0; k < s->doc.nimages; k++)
        if (s->doc.images[k].state == IMG_WAIT &&
            !may_reach(s, doc_str(&s->doc, s->doc.images[k].src))) {
            s->doc.images[k].state = IMG_FAILED;
        } else if (s->doc.images[k].state == IMG_WAIT) {
            s->doc.images[k].state = IMG_LOADING;
            s->pic_k = k;
            fetch_start(&s->pic, doc_str(&s->doc, s->doc.images[k].src), NULL,
                        0, WV_IMG_FILE_MAX);
            return;
        }
}

/* the size a picture is shown at: its own, or width=/height=, made to fit
 * (and to WV_IMG_PIX_MAX) */
static void fit(const wv_image_t *im, int sw, int sh, int room, int *w,
                int *h) {
    int dw = im->aw > 0 ? im->aw : sw, dh = im->ah > 0 ? im->ah : sh;
    if (im->aw > 0 && im->ah <= 0)
        dh = sh * dw / (sw ? sw : 1);
    if (im->ah > 0 && im->aw <= 0)
        dw = sw * dh / (sh ? sh : 1);
    if (dw > room) {
        dh = dh * room / dw;
        dw = room;
    }
    while ((uint32_t)(dw * dh) > WV_IMG_PIX_MAX) {
        dw = dw * 3 / 4;
        dh = dh * 3 / 4;
    }
    *w = dw < 1 ? 1 : dw;
    *h = dh < 1 ? 1 : dh;
}

static void picture_arrived(wv_state_t *s) {
    wv_image_t *im = &s->doc.images[s->pic_k];
    int sw, sh, w = 0, h = 0;
    const char *why = NULL;
    if (s->pic.state == F_DONE && img_size(s->pic.body, s->pic.len, &sw, &sh)) {
        fit(im, sw, sh, view_w(s) - 8, &w, &h);
        im->pix = img_decode(s->pic.body, s->pic.len, w, h, s->doc.bg, &why);
    }
    if (im->pix) {
        bool moved = w != im->w || h != im->h;
        im->w = (int16_t)w;
        im->h = (int16_t)h;
        im->state = IMG_DONE;
        /* the same size as its place: drawn there; else laid out again
         * (not too often: it measures the whole page) */
        if (moved && !(im->aw == w && im->ah == h))
            s->relayout = true;
        wm_invalidate(s->win);
    } else {
        im->state = IMG_FAILED;
    }
    fetch_free(&s->pic);
    next_picture(s);
}

// going to a page

static void stop(wv_state_t *s) {
    fetch_free(&s->page);
    fetch_free(&s->pic);
    if (s->pic_k >= 0 && s->pic_k < s->doc.nimages)
        s->doc.images[s->pic_k].state = IMG_WAIT;
    s->pic_k = -1;
    s->loading = false;
    buttons(s);
}

static void go_post(wv_state_t *s, const char *url, const char *post,
                    int post_len) {
    stop(s);
    char u[WV_URL];
    wv_url_t parts;
    if (url_split(url, &parts))
        strlcpy(u, url, sizeof(u));
    else if (!strncasecmp(url, "gopher.", 7)) /* ("gopher.floodgap.com") */
        snprintf(u, sizeof(u), "gopher://%s", url);
    else if (!strncasecmp(url, "ftp.", 4))
        snprintf(u, sizeof(u), "ftp://%s", url);
    else /* ("frogfind.com": http:// in front) */
        snprintf(u, sizeof(u), "http://%s", url);
    ctl_set_text(s->addr, u);
    /* (a button on one of World View's own pages: it stays where it was) */
    int32_t keep = -1;
    if (!strncasecmp(u, "about:", 6) && !strncasecmp(s->url, u, 9))
        keep = s->scroll;
    fetch_start(&s->page, u, post, post_len, WV_HTML_MAX);
    s->page.may_download = true;
    s->loading = true;
    s->want_scroll = keep >= 0 ? keep : 0;
    buttons(s);
}

static void go(wv_state_t *s, const char *url) { go_post(s, url, NULL, 0); }

/* a file on the disk, as file:/path (pictures and links beside it work) */
static void open_local(wv_state_t *s, const char *path) {
    char u[WV_URL];
    snprintf(u, sizeof(u), "file:%s", path);
    if (s->nhist)
        s->hist[s->at].scroll = s->scroll;
    go(s, u);
}

/* a page (or an error page) to show */
static void show_html(wv_state_t *s, const char *url, char *html,
                      uint32_t len) {
    doc_free(&s->doc);
    s->doc.html = html;
    s->doc.html_len = len;
    s->doc.visited = visited;
    s->doc.user = s;
    strlcpy(s->doc.base, url, sizeof(s->doc.base));
    strlcpy(s->url, url, sizeof(s->url));
    s->focus = -1;
    s->hover = -1;
    s->scroll = s->want_scroll;
    layout(s);
    set_title(s);
    ctl_set_text(s->addr, url);
}

static void error_page(wv_state_t *s, const char *url, const char *why) {
    char *h = (char *)kmalloc(1024);
    if (!h)
        return;
    int n = snprintf(h, 1024,
                     "<title>Can't open the page</title><h2>Can't open the "
                     "page</h2><p>%s<p><tt>%s</tt><hr><p>Try <a href=\"%s\">"
                     "again</a>, or <a href=\"" HOME_URL "\">FrogFind</a>.",
                     why, url, url);
    show_html(s, url, h, (uint32_t)n);
}

/* what came (from the site, or a file): a page, text or a picture shown;
 * anything else kept where the user says. body: taken over */
static void save_download(void *user, const char *path);

static void show_bytes(wv_state_t *s, const char *url, uint8_t *body,
                       uint32_t len, const char *ct, bool truncated) {
    bool html = !ct[0] || strstr(ct, "html") || strstr(ct, "xml");
    if (img_kind(body, len) && !strstr(ct, "html")) {
        /* a picture on its own: a page around it, the bytes already here */
        char *h = (char *)kmalloc(WV_URL + 64);
        if (h) {
            int n = snprintf(h, WV_URL + 64,
                             "<center><img src=\"%s\"></center>", url);
            show_html(s, url, h, (uint32_t)n);
        }
        fetch_free(&s->pic);
        s->pic.body = body;
        s->pic.len = len;
        s->pic.state = F_DONE;
        if (s->doc.nimages) {
            s->pic_k = 0;
            s->doc.images[0].state = IMG_LOADING;
            picture_arrived(s);
        } else {
            fetch_free(&s->pic);
        }
    } else if (html || strstr(ct, "text/")) {
        char *h = (char *)body;
        if (!html) { /* text: as it is (its < and & not read as HTML) */
            uint32_t more = 0;
            for (uint32_t i = 0; i < len; i++)
                more += h[i] == '<' ? 3 : h[i] == '&' ? 4 : 0;
            char *t = (char *)kmalloc(len + more + 16);
            if (t) {
                uint32_t n = 5;
                memcpy(t, "<pre>", 5);
                for (uint32_t i = 0; i < len; i++) {
                    if (h[i] == '<') {
                        memcpy(t + n, "&lt;", 4);
                        n += 4;
                    } else if (h[i] == '&') {
                        memcpy(t + n, "&amp;", 5);
                        n += 5;
                    } else {
                        t[n++] = h[i];
                    }
                }
                t[n] = 0;
                len = n;
                kfree(h);
                h = t;
            }
        }
        show_html(s, url, h, len);
        if (truncated)
            say(s, "The page was too long: cut short.");
        next_picture(s);
    } else { /* a file: kept where the user says */
        const char *name = strrchr(url, '/');
        name = name && name[1] ? name + 1 : "DOWNLOAD";
        char nm[64];
        int k = 0;
        for (; name[k] && name[k] != '?' && k < 63; k++)
            nm[k] = name[k];
        nm[k] = 0;
        s->page.body = body; /* (kept until it is saved) */
        s->page.len = len;
        s->page.truncated = truncated;
        dlg_save_file(s->win, "Save the file", DL_DIR, nm, NULL, save_download,
                      s);
        return;
    }
    if (s->doc.full)
        say(s, "Not enough memory: the page is cut short.");
    else if (s->pic_k < 0 && !truncated)
        say(s, "Done.");
    buttons(s);
}

static void page_arrived(wv_state_t *s) {
    wv_fetch_t *f = &s->page;
    s->loading = false;
    char url[WV_URL];
    strlcpy(url, f->url, sizeof(url));
    if (!s->from_history) { /* a new entry in the history, after this one */
        if (!s->nhist || strcmp(s->hist[s->at].url, url)) {
            if (s->nhist)
                s->at++;
            if (s->at >= WV_HISTORY) {
                memmove(&s->hist[0], &s->hist[1],
                        sizeof(hist_t) * (WV_HISTORY - 1));
                s->at = WV_HISTORY - 1;
            }
            strlcpy(s->hist[s->at].url, url, WV_URL);
            s->nhist = s->at + 1;
        }
    }
    s->from_history = false;
    visit(s, url);
    uint8_t *body = f->body;
    uint32_t len = f->len;
    bool cut = f->truncated;
    char ct[48];
    strlcpy(ct, f->ctype, sizeof(ct));
    f->body = NULL;
    fetch_free(f);
    show_bytes(s, url, body, len, ct, cut);
    list_visit(url, s->doc.title);
}

static void save_download(void *user, const char *path) {
    wv_state_t *s = user;
    if (path && s->page.body) {
        bool ok = A->file_write(path, s->page.body, (int)s->page.len);
        say(s, ok ? (s->page.truncated ? "Saved (but cut short: too big)."
                                       : "Saved.")
                  : "Could not save it.");
    }
    fetch_free(&s->page);
    buttons(s);
}

// downloads: one at a time, beside the pages, into DL_DIR

/* DL_DIR/name, made if it isn't there; a name taken: "name (2).ext" */
static bool dl_path(const char *name, char *out, int sz) {
    app_dirent_t de;
    if (!A->file_info(DL_DIR, &de) && !A->make_dir(DL_DIR))
        return false;
    char base[64];
    strlcpy(base, name, sizeof(base));
    char *dot = strrchr(base, '.');
    const char *ext = "";
    if (dot && dot != base) {
        ext = name + (dot - base);
        *dot = 0;
    }
    snprintf(out, sz, DL_DIR "/%s", name);
    for (int k = 2; A->file_info(out, &de) && k < 100; k++)
        snprintf(out, sz, DL_DIR "/%s (%d)%s", base, k, ext);
    return !A->file_info(out, &de);
}

/* the name a URL's file has: its last part */
static void file_name(const char *url, char *out, int sz) {
    char u[WV_URL];
    strlcpy(u, url, sizeof(u));
    char *q = strchr(u, '?');
    if (q)
        *q = 0;
    int n = (int)strlen(u);
    while (n && u[n - 1] == '/')
        u[--n] = 0;
    const char *name = strrchr(u, '/');
    name = name && name[1] && !strstr(name - 1, "//") ? name + 1 : "DOWNLOAD";
    char nm[WV_URL];
    strlcpy(nm, name, sizeof(nm));
    url_decode(nm, false);
    int k = 0;
    for (int i = 0; nm[i] && k < sz - 1; i++) /* (what a FAT name can have) */
        if (nm[i] != '/' && nm[i] != '\\' && nm[i] != ':' && nm[i] != '"' &&
            (uint8_t)nm[i] >= 32)
            out[k++] = nm[i];
    out[k] = 0;
    if (!k)
        strlcpy(out, "DOWNLOAD", (size_t)sz);
}

/* the download over: kept in the list (rec 'D' done, 'F' failed, 'S'
 * stopped, its half a file deleted; 0 not: cancelled before it had a
 * file), the downloads page shown again if it is the one open */
static void dl_end_as(wv_state_t *s, const char *why, char rec,
                      const char *reason, bool show_again) {
    if (rec)
        list_download(rec, s->dl.save, s->dl.url, s->dl.total, reason);
    fetch_free(&s->dl);
    if (rec && rec != 'D' && s->dl.save[0]) /* (half a file: not kept) */
        A->remove(s->dl.save);
    s->dl_on = false;
    s->dl_asking = false;
    say(s, why);
    buttons(s);
    if (show_again && !s->loading && !strcmp(s->url, "about:downloads")) {
        int32_t at = s->scroll;
        s->from_history = true;
        go(s, "about:downloads");
        s->want_scroll = at;
    }
}

static void dl_end(wv_state_t *s, const char *why, char rec,
                   const char *reason) {
    dl_end_as(s, why, rec, reason, true);
}

/* (the downloads page) */
const wv_fetch_t *wv_download(const char **name) {
    wv_state_t *s = me();
    if (!s || !s->dl_on || s->dl_asking)
        return NULL;
    *name = s->dl_name;
    return &s->dl;
}

void wv_download_stop(void) {
    wv_state_t *s = me();
    if (s && s->dl_on) /* (from the page being made: not made again) */
        dl_end_as(s, "The download was stopped.", 'S', "", false);
}

static void dl_saved(void *user, const char *path) {
    wv_state_t *s = user;
    if (!s->dl_on)
        return;
    s->dl_asking = false;
    if (!path) {
        dl_end(s, "The download was cancelled.", 0, NULL);
        return;
    }
    if (s->dl.state == F_FAILED) {
        char why[128];
        snprintf(why, sizeof(why), "The download stopped: %s", s->dl.err);
        dl_end(s, why, 0, NULL);
        return;
    }
    const char *base = strrchr(path, '/');
    strlcpy(s->dl_name, base ? base + 1 : path, sizeof(s->dl_name));
    if (!fetch_save_to(&s->dl, path))
        dl_end(s, "Could not make the file.", 0, NULL);
}

/* a download asked for: where it goes */
static void dl_ask(wv_state_t *s) {
    file_name(s->dl.url, s->dl_name, sizeof(s->dl_name));
    s->dl_on = true;
    s->dl_asking = true;
    app_dirent_t de;
    if (!A->file_info(DL_DIR, &de))
        A->make_dir(DL_DIR);
    dlg_save_file(s->win, "Save the file", DL_DIR, s->dl_name, NULL, dl_saved,
                  s);
    buttons(s);
}

static void dl_status(wv_state_t *s);

/* the page's fetch turned out a file: it goes on as the download */
static void page_to_download(wv_state_t *s) {
    s->loading = false;
    if (s->dl_on) {
        fetch_free(&s->page);
        say(s, "One download at a time: this one waits till the other is "
               "done.");
        buttons(s);
        return;
    }
    s->dl = s->page;
    memset(&s->page, 0, sizeof(s->page));
    s->page.sock = s->page.dns = s->page.data = -1;
    /* straight into /DOWNLOAD (Save Link As asks where) */
    char name[64], path[96];
    file_name(s->dl.url, name, sizeof(name));
    if (!dl_path(name, path, sizeof(path)) || !fetch_save_to(&s->dl, path)) {
        dl_ask(s); /* (no folder for it: asks) */
        return;
    }
    strlcpy(s->dl_name, path + sizeof(DL_DIR), sizeof(s->dl_name));
    s->dl_on = true;
    s->dl_asking = false;
    buttons(s);
    dl_status(s);
}

/* Save Link As: whatever is there, into a file */
static void save_url(wv_state_t *s, const char *url) {
    if (s->dl_on) {
        say(s, "One download at a time: wait for this one.");
        return;
    }
    fetch_start(&s->dl, url, NULL, 0, WV_HTML_MAX);
    s->dl.save_any = true;
    dl_ask(s);
}

/* how it goes, for the status line */
static void dl_status(wv_state_t *s) {
    char t[160];
    unsigned got = (unsigned)(s->dl.total / 1024);
    if (s->dl.state != F_RECV || s->dl_asking)
        fetch_status(&s->dl, t, sizeof(t));
    else if (s->dl.clen > 0)
        snprintf(t, sizeof(t), "Downloading %s: %u of %u KB", s->dl_name, got,
                 (unsigned)((uint32_t)s->dl.clen / 1024));
    else
        snprintf(t, sizeof(t), "Downloading %s: %u KB", s->dl_name, got);
    if (s->dl_asking && s->dl.state == F_RECV)
        snprintf(t, sizeof(t), "%s: where to save it?", s->dl_name);
    say(s, t);
}

static void dl_frame(wv_state_t *s) {
    int st = fetch_step(&s->dl);
    if (s->dl_asking)
        return; /* (done or not: it finishes once it has its file) */
    char t[160];
    if (st == F_DONE) {
        if (s->dl.save_failed) {
            snprintf(t, sizeof(t), "Could not write %s (is the disk full?)",
                     s->dl_name);
            dl_end(s, t, 'F', "could not write the file");
        } else {
            snprintf(t, sizeof(t), "Saved %s (%u KB).", s->dl.save,
                     (unsigned)((s->dl.total + 1023) / 1024));
            dl_end(s, t, 'D', "");
        }
    } else if (st == F_FAILED) {
        snprintf(t, sizeof(t), "The download stopped: %s", s->dl.err);
        dl_end(s, t, 'F', s->dl.err);
    } else if (!s->loading && s->pic_k < 0 && s->hover < 0) {
        dl_status(s);
    }
}

// history

static void history_go(wv_state_t *s, int to) {
    if (to < 0 || to >= s->nhist)
        return;
    s->hist[s->at].scroll = s->scroll;
    s->at = to;
    s->from_history = true;
    go(s, s->hist[to].url);
    s->want_scroll = s->hist[to].scroll;
    buttons(s);
}

// fields

static wv_field_t *field(wv_state_t *s, int k) {
    return k >= 0 && k < s->doc.nfields ? &s->doc.fields[k] : NULL;
}

static void submit(wv_state_t *s, int form, int button) {
    if (form < 0 || form >= s->doc.nforms)
        return;
    char *q = (char *)kmalloc(2048);
    if (!q)
        return;
    int n = doc_form_query(&s->doc, form, button, q, 2048);
    const char *action = doc_str(&s->doc, s->doc.forms[form].action);
    if (!may_reach(s, action)) {
        kfree(q);
        say(s, "A web page can't send that to World View's own pages.");
        return;
    }
    if (s->doc.forms[form].post) {
        go_post(s, action, q, n);
    } else {
        char url[WV_URL];
        strlcpy(url, action, sizeof(url));
        char *qm = strchr(url, '?');
        if (qm)
            *qm = 0;
        strlcat(url, "?", sizeof(url));
        strlcat(url, q, sizeof(url));
        go(s, url);
    }
    kfree(q);
}

static void reset_form(wv_state_t *s, int form) {
    for (int k = 0; k < s->doc.nfields; k++) {
        wv_field_t *f = &s->doc.fields[k];
        if (f->form != form)
            continue;
        if (f->type == FLD_TEXT || f->type == FLD_PASSWORD ||
            f->type == FLD_TEXTAREA)
            strlcpy(f->value, doc_str(&s->doc, f->initial), sizeof(f->value));
    }
    wm_invalidate(s->win);
}

/* a select's options: a menu */
#define OPT_MAX 16
static void pick_opt(int i) {
    wv_state_t *s = me();
    wv_field_t *f = s ? field(s, s->menu_field) : NULL;
    if (f && i < f->nopt) {
        f->sel = (int16_t)i;
        wm_invalidate(s->win);
    }
}
/* (a menu item calls a function of nothing: one for each option) */
#define OPT(n)                                                                 \
    static void opt##n(void) { pick_opt(n); }
// clang-format off
OPT(0) OPT(1) OPT(2) OPT(3) OPT(4) OPT(5) OPT(6) OPT(7)
OPT(8) OPT(9) OPT(10) OPT(11) OPT(12) OPT(13) OPT(14) OPT(15)
    // clang-format on
    static void (*const OPTS[OPT_MAX])(void) = {
        opt0, opt1, opt2,  opt3,  opt4,  opt5,  opt6,  opt7,
        opt8, opt9, opt10, opt11, opt12, opt13, opt14, opt15};

static const char *opt_label(const wv_doc_t *d, const wv_field_t *f, int i) {
    const char *p = doc_str(d, f->opts);
    for (int k = 0; k < i; k++) {
        p += strlen(p) + 1;
        p += strlen(p) + 1;
    }
    return p + strlen(p) + 1;
}

static void field_click(wv_state_t *s, int k) {
    wv_field_t *f = field(s, k);
    if (!f)
        return;
    s->focus = -1;
    switch (f->type) {
    case FLD_TEXT:
    case FLD_PASSWORD:
    case FLD_TEXTAREA:
        s->focus = k;
        break;
    case FLD_CHECK:
        f->checked = !f->checked;
        break;
    case FLD_RADIO:
        for (int i = 0; i < s->doc.nfields; i++) {
            wv_field_t *o = &s->doc.fields[i];
            if (o->type == FLD_RADIO && o->form == f->form &&
                !strcmp(doc_str(&s->doc, o->name), doc_str(&s->doc, f->name)))
                o->checked = false;
        }
        f->checked = true;
        break;
    case FLD_SUBMIT:
        submit(s, f->form, k);
        return;
    case FLD_RESET:
        reset_form(s, f->form);
        break;
    case FLD_SELECT: {
        s->menu_field = k;
        menu *m = popup_menu();
        for (int i = 0; i < f->nopt && i < OPT_MAX; i++)
            menu_add_item(m, (char *)opt_label(&s->doc, f, i), OPTS[i]);
        popup_show_for(s->view);
        break;
    }
    }
    wm_invalidate(s->win);
}

/* typing in the field that has the focus */
static bool field_key(wv_state_t *s) {
    wv_field_t *f = field(s, s->focus);
    if (!f)
        return false;
    int n = (int)strlen(f->value);
    uint8_t sc = kb.last_scancode;
    char c = kb.last_char;
    if (sc == ENTER) {
        if (f->type == FLD_TEXTAREA && n < WV_FIELD_TEXT - 1) {
            f->value[n] = ' ';
            f->value[n + 1] = 0;
        } else {
            int button = -1; /* (the form's first button sends it) */
            for (int k = 0; k < s->doc.nfields && button < 0; k++)
                if (s->doc.fields[k].form == f->form &&
                    s->doc.fields[k].type == FLD_SUBMIT)
                    button = k;
            submit(s, f->form, button);
        }
    } else if (sc == BACKSPACE) {
        if (n)
            f->value[n - 1] = 0;
    } else if (sc == ESC) {
        s->focus = -1;
    } else if (sc == TAB) {
        int k = s->focus + 1;
        while (k < s->doc.nfields && s->doc.fields[k].type != FLD_TEXT &&
               s->doc.fields[k].type != FLD_PASSWORD &&
               s->doc.fields[k].type != FLD_TEXTAREA)
            k++;
        s->focus = k < s->doc.nfields ? k : -1;
    } else if ((uint8_t)c >= 32 && n < WV_FIELD_TEXT - 1 && !kb.ctrl) {
        f->value[n] = c;
        f->value[n + 1] = 0;
    } else {
        return false;
    }
    s->blink = time_millis();
    wm_invalidate(s->win);
    return true;
}

// drawing the page

static void bevel(int x, int y, int w, int h, bool raised) {
    uint8_t lt = raised ? WHITE : DARK_GRAY, dk = raised ? DARK_GRAY : WHITE;
    fill_rect(x, y, w, 1, lt);
    fill_rect(x, y, 1, h, lt);
    fill_rect(x, y + h - 1, w, 1, dk);
    fill_rect(x + w - 1, y, 1, h, dk);
}

static void draw_field(wv_state_t *s, const wv_item_t *it, int x, int y) {
    wv_field_t *f = field(s, it->ref);
    if (!f)
        return;
    int w = it->w, h = it->h;
    switch (f->type) {
    case FLD_SUBMIT:
    case FLD_RESET: {
        fill_rect(x, y, w, h, LIGHT_GRAY);
        bevel(x, y, w, h, true);
        const char *label = f->type == FLD_SUBMIT && it->ref >= 0
                                ? f->value
                                : doc_str(&s->doc, f->initial);
        if (!label[0])
            label = f->value;
        int tw = wv_text_w(FONT_SMALL, 1, label, (int)strlen(label));
        draw_text_n(x + (w - tw) / 2, y + 3, label, (int)strlen(label), BLACK,
                    FONT_SMALL);
        break;
    }
    case FLD_CHECK:
    case FLD_RADIO:
        fill_rect(x, y, w, h, WHITE);
        draw_rect(x, y, w, h, BLACK);
        if (f->checked)
            fill_rect(x + 2, y + 2, w - 4, h - 4, BLACK);
        break;
    case FLD_SELECT: {
        fill_rect(x, y, w, h, WHITE);
        draw_rect(x, y, w, h, BLACK);
        fill_rect(x + w - 11, y + 1, 10, h - 2, LIGHT_GRAY);
        for (int i = 0; i < 3; i++) /* (the arrow) */
            fill_rect(x + w - 8 + i, y + 5 + i, 5 - 2 * i, 1, BLACK);
        if (f->nopt) {
            const char *l = opt_label(&s->doc, f, f->sel);
            draw_text_n(x + 3, y + 3, l, (int)strlen(l), BLACK, FONT_SMALL);
        }
        break;
    }
    default: { /* text, password, textarea */
        fill_rect(x, y, w, h, WHITE);
        bevel(x, y, w, h, false);
        draw_rect(x + 1, y + 1, w - 2, h - 2, BLACK);
        const char *v = f->value;
        char stars[WV_FIELD_TEXT];
        if (f->type == FLD_PASSWORD) {
            int n = (int)strlen(v);
            memset(stars, '*', (uint32_t)n);
            stars[n] = 0;
            v = stars;
        }
        int n = (int)strlen(v), from = 0;
        while (from < n && wv_text_w(FONT_SMALL, 1, v + from, n - from) > w - 8)
            from++; /* (the end shown, where typing goes) */
        draw_text_n(x + 3, y + 3, v + from, n - from, BLACK, FONT_SMALL);
        if (s->focus == it->ref &&
            ((time_millis() - s->blink) / 500) % 2 == 0) {
            int cx = x + 3 + wv_text_w(FONT_SMALL, 1, v + from, n - from);
            fill_rect(cx, y + 2, 1, h - 4, BLACK);
        }
    }
    }
}

static void paint_view(control *c, int x, int y, int w, int h) {
    wv_state_t *s = c->user;
    wv_doc_t *d = &s->doc;
    fill_rect(x, y, w, h, d->bg ? d->bg : WHITE);
    if (!d->items)
        return;
    int32_t top = s->scroll;
    for (int i = doc_item_at_y(d, top); i < d->nitems; i++) {
        const wv_item_t *it = &d->items[i];
        if (it->y > top + h)
            break;
        if (it->y + it->h < top)
            continue;
        int ix = x + it->x, iy = y + (int)(it->y - top);
        switch (it->kind) {
        case IT_TEXT: {
            const char *t = doc_str(d, it->text);
            uint8_t color = it->color;
            if (it->link >= 0 && it->link == s->hover)
                color = RED;
            wv_draw_text(ix, iy, t, it->len, color, it->font, it->scale);
            if (it->flags & IF_UNDER)
                fill_rect(ix, iy + it->h, it->w, 1, color);
            if (it->flags & IF_STRIKE)
                fill_rect(ix, iy + it->h / 2, it->w, 1, color);
            break;
        }
        case IT_BOX:
            fill_rect(ix, iy, it->w, it->h, it->color);
            break;
        case IT_IMAGE: {
            const wv_image_t *im = &d->images[it->ref];
            if (im->pix && im->w <= it->w + 1) {
                draw_pixels(ix, iy, im->w, im->h < it->h ? im->h : it->h,
                            im->pix, im->w);
            } else {
                draw_rect(ix, iy, it->w, it->h, DARK_GRAY);
                const char *alt = doc_str(d, im->alt);
                if (alt[0] && it->w > 8)
                    draw_text_n(ix + 3, iy + 3, alt, (int)strlen(alt),
                                DARK_GRAY, FONT_SMALL);
                else if (it->w > 8 && it->h > 8) /* (a picture to come) */
                    draw_line(ix + 2, iy + it->h - 3, ix + it->w - 3, iy + 2,
                              LIGHT_GRAY);
            }
            if (it->link >= 0)
                draw_rect(ix - 1, iy - 1, it->w + 2, it->h + 2,
                          it->link == s->hover ? RED : d->link_c);
            break;
        }
        case IT_FIELD:
            draw_field(s, it, ix, iy);
            break;
        }
    }
}

/* what is at (x, y) of the page: an item, -1 none */
static int item_at(wv_state_t *s, int x, int32_t y) {
    wv_doc_t *d = &s->doc;
    for (int i = doc_item_at_y(d, y); i < d->nitems; i++) {
        const wv_item_t *it = &d->items[i];
        if (it->y > y)
            break;
        if (x >= it->x && x < it->x + it->w && y >= it->y &&
            y < it->y + it->h + 2)
            return i;
    }
    return -1;
}

static void follow(wv_state_t *s, int link) {
    if (link < 0 || link >= s->doc.nlinks)
        return;
    const char *u = doc_str(&s->doc, s->doc.links[link]);
    wv_url_t parts;
    if (!url_split(u, &parts)) {
        say(s, "World View opens http://, https://, gopher:// and ftp:// "
               "only.");
        return;
    }
    if (!may_reach(s, u)) {
        say(s, "A web page can't open files on this computer.");
        return;
    }
    s->hist[s->at].scroll = s->scroll;
    go(s, u);
}

/* the page you're on, into the bookmarks */
static void add_bookmark(wv_state_t *s) {
    if (!s->url[0] || !strncasecmp(s->url, "about:", 6)) {
        say(s, "This page can't be a bookmark.");
        return;
    }
    say(s, list_bookmark(s->url, s->doc.title)
               ? "Added to the bookmarks (Bookmarks > Show Bookmarks)."
               : "It is in the bookmarks already.");
}

/* one of World View's own pages, scrolled where it was if it is open */
static void own_page(wv_state_t *s, const char *url) {
    s->hist[s->at].scroll = s->scroll;
    go(s, url);
}

// the right-click menu

/* the link of the item right-clicked, or its picture's address */
static const char *menu_link(wv_state_t *s) {
    int i = s->menu_item;
    if (i < 0 || i >= s->doc.nitems || s->doc.items[i].link < 0)
        return NULL;
    return doc_str(&s->doc, s->doc.links[s->doc.items[i].link]);
}

static const char *menu_picture(wv_state_t *s) {
    int i = s->menu_item;
    if (i < 0 || i >= s->doc.nitems || s->doc.items[i].kind != IT_IMAGE)
        return NULL;
    return doc_str(&s->doc, s->doc.images[s->doc.items[i].ref].src);
}

static void ctx_open(void) {
    wv_state_t *s = me();
    if (s && menu_link(s))
        follow(s, s->doc.items[s->menu_item].link);
}
static void ctx_save_link(void) {
    wv_state_t *s = me();
    if (s && menu_link(s))
        save_url(s, menu_link(s));
}
static void ctx_copy_link(void) {
    wv_state_t *s = me();
    if (s && menu_link(s)) {
        clipboard_set_text(menu_link(s), (int)strlen(menu_link(s)));
        say(s, "The address is on the clipboard.");
    }
}
static void ctx_view_pic(void) {
    wv_state_t *s = me();
    if (s && menu_picture(s)) {
        s->hist[s->at].scroll = s->scroll;
        char u[WV_URL];
        strlcpy(u, menu_picture(s), sizeof(u));
        go(s, u);
    }
}
static void ctx_save_pic(void) {
    wv_state_t *s = me();
    if (s && menu_picture(s))
        save_url(s, menu_picture(s));
}
static void ctx_copy_pic(void) {
    wv_state_t *s = me();
    if (s && menu_picture(s)) {
        clipboard_set_text(menu_picture(s), (int)strlen(menu_picture(s)));
        say(s, "The picture's address is on the clipboard.");
    }
}
static void ctx_back(void) {
    wv_state_t *s = me();
    if (s)
        history_go(s, s->at - 1);
}
static void ctx_reload(void) {
    wv_state_t *s = me();
    if (s && s->url[0]) {
        s->hist[s->at].scroll = s->scroll;
        s->from_history = true;
        go(s, s->url);
        s->want_scroll = s->scroll;
    }
}
static void ctx_copy_page(void) {
    wv_state_t *s = me();
    if (s && s->url[0]) {
        clipboard_set_text(s->url, (int)strlen(s->url));
        say(s, "The page's address is on the clipboard.");
    }
}

static void context_menu(wv_state_t *s, control *c) {
    s->menu_item = item_at(s, c->mx - 2, s->scroll + c->my - 2);
    menu *m = popup_menu();
    bool any = false;
    if (menu_link(s)) {
        menu_add_item(m, "Open Link", ctx_open);
        menu_add_item(m, "Save Link As...", ctx_save_link);
        menu_add_item(m, "Copy Link Address", ctx_copy_link);
        any = true;
    }
    if (menu_picture(s)) {
        if (any)
            menu_add_separator(m);
        menu_add_item(m, "View Picture", ctx_view_pic);
        menu_add_item(m, "Save Picture As...", ctx_save_pic);
        menu_add_item(m, "Copy Picture Address", ctx_copy_pic);
        any = true;
    }
    if (!any) {
        menu_add_item(m, "Back", ctx_back);
        menu_enable_last(m, s->at > 0);
        menu_add_item(m, "Reload", ctx_reload);
        menu_add_item(m, "Copy Page Address", ctx_copy_page);
    }
    popup_show_for(c);
}

static void scroll_by(wv_state_t *s, int32_t dy) {
    s->scroll += dy;
    clamp_scroll(s);
    wm_invalidate(s->win);
}

static void on_view(control *c, int ev) {
    wv_state_t *s = c->user;
    if (ev == EV_MOUSEDOWN) {
        int i = item_at(s, c->mx - 2, s->scroll + c->my - 2);
        const wv_item_t *it = i >= 0 ? &s->doc.items[i] : NULL;
        if (it && it->kind == IT_FIELD) {
            field_click(s, it->ref);
        } else if (it && it->link >= 0) {
            follow(s, it->link);
        } else if (s->focus >= 0) {
            s->focus = -1;
            wm_invalidate(s->win);
        }
    } else if (ev == EV_MENU) {
        context_menu(s, c);
    } else if (ev == EV_KEY) {
        if (s->focus >= 0 && field_key(s))
            return;
        if (kb.ctrl) { /* Ctrl+D bookmark, B bookmarks, H history, J
                          downloads */
            switch (kb.last_scancode) {
            case 0x20:
                add_bookmark(s);
                return;
            case 0x30:
                own_page(s, "about:bookmarks");
                return;
            case 0x23:
                own_page(s, "about:history");
                return;
            case 0x24:
                own_page(s, "about:downloads");
                return;
            }
        }
        int vh = view_h(s);
        switch (kb.last_scancode) {
        case UP_ARROW:
            scroll_by(s, -16);
            break;
        case DOWN_ARROW:
            scroll_by(s, 16);
            break;
        case PAGE_UP:
            scroll_by(s, -(vh - 16));
            break;
        case PAGE_DOWN:
        case SPACE:
            scroll_by(s, vh - 16);
            break;
        case HOME:
            scroll_by(s, -s->doc.height);
            break;
        case END:
            scroll_by(s, s->doc.height);
            break;
        case BACKSPACE:
            history_go(s, s->at - 1);
            break;
        }
    }
}

static void on_vs(control *c, int ev) {
    wv_state_t *s = c->user;
    (void)ev;
    s->scroll = ctl_value(c);
    wm_invalidate(s->win);
}

// the toolbar, menus

static void on_addr(control *c, int ev) {
    wv_state_t *s = c->user;
    if (ev == EV_SUBMIT && ctl_text(c)[0]) {
        go(s, ctl_text(c));
        ctl_focus(s->view);
    }
}

static void on_btn(control *c, int ev) {
    wv_state_t *s = c->user;
    (void)ev;
    switch (c->id) {
    case 0:
        history_go(s, s->at - 1);
        break;
    case 1:
        history_go(s, s->at + 1);
        break;
    case 2:
        if (s->url[0]) {
            s->hist[s->at].scroll = s->scroll;
            s->from_history = true;
            go(s, s->url);
            s->want_scroll = s->scroll;
        }
        break;
    case 3:
        if (!s->loading && s->pic_k < 0 && s->dl_on) {
            dl_end(s, "The download was stopped.", 'S', "");
            break;
        }
        stop(s);
        say(s, "Stopped.");
        break;
    case 4:
        go(s, s->home);
        break;
    }
}

#define MENU_ACTION(name, body)                                                \
    static void name(void) {                                                   \
        wv_state_t *s = me();                                                  \
        if (s) {                                                               \
            body;                                                              \
        }                                                                      \
    }

static void saved_page(void *user, const char *path) {
    wv_state_t *s = user;
    if (path && s->doc.html)
        say(s, A->file_write(path, s->doc.html, (int)s->doc.html_len)
                   ? "Saved."
                   : "Could not save it.");
}

static void home_set(void *user, const char *text) {
    wv_state_t *s = user;
    if (!text)
        return;
    strlcpy(s->home, text[0] ? text : HOME_URL, sizeof(s->home));
    app_prefs_set("WorldView", "Home", s->home);
}

static void proxy_set(void *user, const char *text) {
    wv_state_t *s = user;
    if (!text)
        return;
    strlcpy(g_wv_net.proxy, text, sizeof(g_wv_net.proxy));
    app_prefs_set("WorldView", "Proxy", g_wv_net.proxy);
    say(s, g_wv_net.proxy[0] ? "Pages now come through the proxy."
                             : "No proxy: straight to the sites.");
}

MENU_ACTION(menu_open, ctl_focus(s->addr))
MENU_ACTION(menu_save, dlg_save_file(s->win, "Save the page", DL_DIR, "PAGE",
                                     "HTM", saved_page, s))
MENU_ACTION(menu_close, os_quit_app_by_desc(&worldview_app))
MENU_ACTION(menu_back, history_go(s, s->at - 1))
MENU_ACTION(menu_fwd, history_go(s, s->at + 1))
MENU_ACTION(menu_home, go(s, s->home))
MENU_ACTION(menu_reload, on_btn(s->btn[2], EV_CLICK))
MENU_ACTION(menu_stop, on_btn(s->btn[3], EV_CLICK))
MENU_ACTION(menu_frogfind, go(s, HOME_URL))
MENU_ACTION(menu_bookmark, add_bookmark(s))
MENU_ACTION(menu_bookmarks, own_page(s, "about:bookmarks"))
MENU_ACTION(menu_history, own_page(s, "about:history"))
MENU_ACTION(menu_downloads, own_page(s, "about:downloads"))
MENU_ACTION(menu_68k, go(s, "http://68k.news/"))
MENU_ACTION(menu_home_set, dlg_input(s->win, "Home Page",
                                     "The page World View opens with:", s->home,
                                     WV_URL - 1, 0, home_set, s))
MENU_ACTION(menu_proxy,
            dlg_input(s->win, "Proxy",
                      "A proxy (host:port) fetches pages for World View: "
                      "https:// sites need one. Empty: none.",
                      g_wv_net.proxy, 79, 0, proxy_set, s))
MENU_ACTION(menu_images, {
    s->images = !s->images;
    app_prefs_set_int("WorldView", "Images", s->images);
    say(s, s->images ? "Pictures on." : "Pictures off.");
    if (s->images && !s->loading && s->pic_k < 0)
        next_picture(s);
})
MENU_ACTION(menu_about,
            dlg_message(s->win, "About World View",
                        "World View 1.1\nA web browser for the web old "
                        "machines can read: FrogFind, 68k.news, plain HTTP "
                        "sites, gopher:// and ftp:// servers. HTML, forms, "
                        "GIF, BMP and JPEG pictures, downloads.\n"
                        "Secure (https) sites: through a proxy (Options)."))

// the app

static void wv_init(void *state) {
    wv_state_t *s = state;
    S = s;
    int sw = shim_screen_w(), sh = shim_screen_h();
    int w = sw >= 640 ? 600 : sw - 8, h = sh >= 400 ? 360 : sh - 30;
    s->win =
        app_window(s, "World View", (sw - w) / 2, 14, w, h, BLUE, AW_RESIZABLE);
    if (!s->win)
        return;
    s->win->min_w = 200;
    s->win->min_h = 120;
    void *room = kmalloc(256u * 1024u); /* (the loader gives less when */
    if (!room) {                        /*  memory is short)           */
        dlg_message(s->win, "World View",
                    "Not enough memory: World View needs about 640 KB free. "
                    "Close other apps and try again.");
        os_quit_app_by_desc(&worldview_app);
        return;
    }
    kfree(room);
    img_palette_init();
    s->pic_k = -1;
    s->focus = -1;
    s->hover = -1;
    s->page.sock = s->page.dns = s->pic.sock = s->pic.dns = -1;
    s->page.data = s->pic.data = s->dl.sock = s->dl.dns = s->dl.data = -1;
    s->menu_item = -1;
    if (!app_prefs_get("WorldView", "Home", s->home, sizeof(s->home)) ||
        !s->home[0])
        strcpy(s->home, HOME_URL);
    app_prefs_get("WorldView", "Proxy", g_wv_net.proxy, sizeof(g_wv_net.proxy));
    s->images = app_prefs_get_int("WorldView", "Images", 1) != 0;

    int cx, cy, cw, ch;
    win_client_rect(s->win, &cx, &cy, &cw, &ch);
    static const char *const LABEL[5] = {"<", ">", "R", "X", "H"};
    static const char *const TIP[5] = {"Back", "Forward", "Reload", "Stop",
                                       "Home"};
    for (int i = 0; i < 5; i++) {
        s->btn[i] =
            ctl_button(s->win, 2 + i * 15, 2, 14, 13, LABEL[i], i, on_btn);
        ctl_set_tooltip(s->btn[i], TIP[i]);
        ctl_style_add(s->btn[i], CS_NOFOCUS);
    }
    s->addr = ctl_edit(s->win, 80, 2, cw - 82, 13, "", WV_URL - 1, CS_STRETCH_W,
                       on_addr);
    s->status = ctl_status(s->win, "");
    int vh = ch - TOOL_H - 12, vsw = 8; /* (a scroll bar: 8 across) */
    s->vs = ctl_vscroll(s->win, cw - vsw, TOOL_H, vh, on_vs);
    ctl_style_add(s->vs, CS_ANCHOR_RIGHT | CS_STRETCH_H);
    s->view =
        ctl_custom(s->win, 0, TOOL_H, cw - vsw, vh, paint_view, on_view,
                   CS_BORDER | CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);

    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "Open Location...", menu_open);
    menu_add_item(f, "Save Page As...", menu_save);
    menu_add_separator(f);
    menu_add_item(f, "Close", menu_close);
    menu *g = window_add_menu(s->win, "Go");
    menu_add_item(g, "Back", menu_back);
    menu_add_item(g, "Forward", menu_fwd);
    menu_add_item(g, "Home", menu_home);
    menu_add_item(g, "Reload", menu_reload);
    menu_add_item(g, "Stop", menu_stop);
    menu_add_separator(g);
    menu_add_item(g, "History", menu_history);
    menu_add_item(g, "Downloads", menu_downloads);
    menu_add_separator(g);
    menu_add_item(g, "FrogFind", menu_frogfind);
    menu_add_item(g, "68k.news", menu_68k);
    menu *bm = window_add_menu(s->win, "Bookmarks");
    menu_add_item(bm, "Add Bookmark", menu_bookmark);
    menu_add_item(bm, "Show Bookmarks", menu_bookmarks);
    menu *o = window_add_menu(s->win, "Options");
    menu_add_item(o, "Home Page...", menu_home_set);
    menu_add_item(o, "Proxy...", menu_proxy);
    menu_add_item(o, "Pictures On/Off", menu_images);
    menu_add_separator(o);
    menu_add_item(o, "About World View", menu_about);

    g_wv_about = list_page;
    char start[WV_URL];
    if (app_open_request(start, sizeof(start)) && start[0])
        open_local(s, start); /* (a .HTM file, from FileF) */
    else
        go(s, s->home);
    buttons(s);
    ctl_focus(s->view);
}

static void wv_frame(void *state) {
    wv_state_t *s = state;
    if (!s->win)
        return;
    S = s;
    char t[160];
    /* a .HTM opened in FileF while World View is running: shown here */
    char req[WV_URL];
    if (app_open_request(req, sizeof(req)) && req[0])
        open_local(s, req);
    if (s->loading) {
        int st = fetch_step(&s->page);
        if (s->page.download) {
            page_to_download(s);
        } else if (st == F_DONE) {
            page_arrived(s);
        } else if (st == F_FAILED) {
            s->loading = false;
            char why[96];
            strlcpy(why, s->page.err, sizeof(why));
            char url[WV_URL];
            strlcpy(url, s->page.url, sizeof(url));
            fetch_free(&s->page);
            error_page(s, url, why);
            say(s, why);
            buttons(s);
        } else {
            fetch_status(&s->page, t, sizeof(t));
            say(s, t);
        }
    } else if (s->pic_k >= 0) {
        int st = fetch_step(&s->pic);
        if (st == F_DONE || st == F_FAILED) {
            picture_arrived(s);
        } else {
            int left = 0;
            for (int k = 0; k < s->doc.nimages; k++)
                left += s->doc.images[k].state <= IMG_LOADING;
            snprintf(t, sizeof(t), "Pictures: %d to come", left);
            say(s, t);
        }
        if (s->pic_k < 0) {
            say(s, s->doc.full ? "Done (the page was cut short)." : "Done.");
            buttons(s);
        }
    }
    if (s->dl_on)
        dl_frame(s);
    /* laid out again: the window's width changed, or pictures came (at
     * most once a second while they come) */
    uint32_t now = time_millis();
    if (view_w(s) != s->laid_w ||
        (s->relayout && (s->pic_k < 0 || now - s->relayout_at > 1000))) {
        int32_t keep = s->scroll;
        layout(s);
        s->scroll = keep;
        clamp_scroll(s);
        s->relayout_at = now;
    }
    if (!s->loading && s->want_scroll && s->doc.height > s->want_scroll) {
        s->scroll = s->want_scroll;
        s->want_scroll = 0;
        clamp_scroll(s);
    }
    /* the mouse over a link: the hand, and where it goes */
    control *v = s->view;
    int hover = -1;
    if (window_is_focused(s->win) && mouse.x >= v->ax + 2 &&
        mouse.y >= v->ay + 2 && mouse.x < v->ax + v->aw - 2 &&
        mouse.y < v->ay + v->ah - 2) {
        int i =
            item_at(s, mouse.x - v->ax - 2, s->scroll + mouse.y - v->ay - 2);
        if (i >= 0 && s->doc.items[i].link >= 0) {
            hover = s->doc.items[i].link;
            A->pointer(APP_POINTER_HAND);
        } else if (i >= 0 && s->doc.items[i].kind == IT_FIELD) {
            wv_field_t *f = field(s, s->doc.items[i].ref);
            if (f && (f->type == FLD_TEXT || f->type == FLD_PASSWORD ||
                      f->type == FLD_TEXTAREA))
                A->pointer(APP_POINTER_TEXT);
            else
                A->pointer(APP_POINTER_HAND);
        }
    }
    if (hover != s->hover) {
        s->hover = hover;
        if (hover >= 0)
            say(s, doc_str(&s->doc, s->doc.links[hover]));
        else if (!s->loading && s->pic_k < 0 && !s->dl_on)
            say(s, "Done.");
        wm_invalidate(s->win);
    }
    if (s->focus >= 0) /* (the caret blinks) */
        wm_invalidate(s->win);
}

static void wv_destroy(void *state) {
    wv_state_t *s = state;
    fetch_free(&s->page);
    fetch_free(&s->pic);
    fetch_free(&s->dl);
    doc_free(&s->doc);
    if (S == s)
        S = NULL;
}

APP_META("World View", wv_state_t, true, 640, wv_init, wv_frame, wv_destroy);
