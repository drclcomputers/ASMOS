/* Displays /HELP pages by app. Plain text supports "# " headings, "- " list
 * items, blank-line paragraphs, and wrapped paragraph lines. */
#include "os/help.h"
#include "fs/fs.h"
#include "os/api.h"
#include "ui/uifont.h"

#define MAX_NODES 200
#define REL_SZ 112     /* "Folder/Page.TXT" */
#define PAGE_MAX 16384 /* bytes of a page read */
#define MAX_LINES 900
#define TREE_W 92
#define PAD 4

enum { L_TEXT, L_HEAD, L_BULLET, L_GAP };

typedef struct {
    uint16_t off, len;  /* in text[] */
    uint8_t kind, cont; /* cont: a bullet's later lines (no dot) */
} line_t;

typedef struct {
    window *win; /* must stay first */
    control *tree, *view, *sb;
    int folders[64], nfolders;   /* the top nodes (an app each) */
    char rel[MAX_NODES][REL_SZ]; /* tree data -> its path under /HELP */
    int nrel;
    char shown[REL_SZ]; /* the page shown */
    char *raw;          /* its text, as read */
    char *text;         /* the same, laid out (the lines point into it) */
    line_t lines[MAX_LINES];
    int nlines, laid_w, top;
    uint32_t fs_rev; /* /HELP as last listed */
    uint32_t sig;
} help_state_t;

app_descriptor help_app;
static char s_pending[FS_PATH_SZ]; /* help_open: where to go */
static bool s_has_pending;

// names

/* "03_Getting_started.TXT" -> "Getting started"; "Audio_Player" -> ... */
static void shown_name(const char *n, char *out, int sz) {
    const char *p = n;
    while (*p >= '0' && *p <= '9')
        p++;
    if (p != n && *p == '_')
        p++;
    else
        p = n;
    int k = 0;
    for (; *p && k < sz - 1; p++) {
        if (*p == '.' && !strchr(p + 1, '.'))
            break; /* the extension */
        out[k++] = *p == '_' ? ' ' : *p;
    }
    out[k] = 0;
}

// the list of folders and pages

typedef struct {
    char names[64][FS_NAME_SZ];
    bool dir[64];
    int n;
} listing_t;

static bool list_cb(const dir_entry_t *e, const char *name, void *user) {
    listing_t *l = user;
    if (name[0] == '.' || (e->attr & (ATTR_VOLUME_ID | ATTR_HIDDEN)) ||
        l->n >= 64)
        return l->n < 64;
    snprintf(l->names[l->n], FS_NAME_SZ, "%s", name);
    l->dir[l->n++] = (e->attr & ATTR_DIRECTORY) != 0;
    return true;
}

/* a folder's entries, sorted by name (ASMOS first at the top) */
static void list_sorted(const char *path, listing_t *l) {
    l->n = 0;
    uint8_t drive;
    uint16_t cl;
    if (fs_resolve_dir(path, &drive, &cl))
        fs_walk_dir(drive, cl, list_cb, l);
    for (int i = 1; i < l->n; i++)
        for (int k = i; k > 0; k--) {
            bool a_first = !strcasecmp(l->names[k], "ASMOS");
            if (!a_first && (!strcasecmp(l->names[k - 1], "ASMOS") ||
                             strcasecmp(l->names[k], l->names[k - 1]) >= 0))
                break;
            char t[FS_NAME_SZ];
            bool d = l->dir[k];
            memcpy(t, l->names[k], FS_NAME_SZ);
            memcpy(l->names[k], l->names[k - 1], FS_NAME_SZ);
            memcpy(l->names[k - 1], t, FS_NAME_SZ);
            l->dir[k] = l->dir[k - 1];
            l->dir[k - 1] = d;
        }
}

static int add_rel(help_state_t *s, const char *rel) {
    if (s->nrel >= MAX_NODES)
        return -1;
    snprintf(s->rel[s->nrel], REL_SZ, "%s", rel);
    return s->nrel++;
}

static uint32_t hash(uint32_t h, const char *t) {
    for (; *t; t++)
        h = h * 31 + (uint8_t)*t;
    return h;
}

/* the tree from /HELP; returns its signature (to see it changed) */
static uint32_t fill_tree(help_state_t *s, bool build) {
    static listing_t top, in;
    list_sorted(HELP_DIR, &top);
    uint32_t sig = 7;
    if (build) {
        tree_clear(s->tree);
        s->nrel = 0;
        s->nfolders = 0;
    }
    for (int i = 0; i < top.n; i++) {
        if (!top.dir[i])
            continue;
        char p[FS_PATH_SZ], label[48];
        snprintf(p, sizeof(p), "%s/%s", HELP_DIR, top.names[i]);
        list_sorted(p, &in);
        sig = hash(sig, top.names[i]);
        int folder = -1;
        if (build) {
            shown_name(top.names[i], label, sizeof(label));
            int r = add_rel(s, top.names[i]);
            folder = tree_add(s->tree, -1, label, (uint32_t)r, false);
            if (folder >= 0 && s->nfolders < 64)
                s->folders[s->nfolders++] = folder;
        }
        for (int k = 0; k < in.n; k++) {
            if (in.dir[k])
                continue;
            sig = hash(sig, in.names[k]);
            if (!build || folder < 0)
                continue;
            char rel[REL_SZ];
            snprintf(rel, sizeof(rel), "%s/%s", top.names[i], in.names[k]);
            int r = add_rel(s, rel);
            if (r < 0)
                break;
            shown_name(in.names[k], label, sizeof(label));
            tree_add(s->tree, folder, label, (uint32_t)r, false);
        }
    }
    return sig;
}

/* the tree node for a path under /HELP ("FileF" or "FileF/01_X.TXT") */
static int node_of(help_state_t *s, const char *rel) {
    for (int i = 0; i < s->nfolders; i++) {
        int n = s->folders[i];
        if (!strcasecmp(s->rel[tree_data(s->tree, n)], rel))
            return n;
        for (int c = tree_first_child(s->tree, n); c >= 0;
             c = tree_next_sibling(s->tree, c))
            if (!strcasecmp(s->rel[tree_data(s->tree, c)], rel))
                return c;
    }
    return -1;
}

// the page

static int text_w(const char *t, int n, int f) { return text_width_n(t, n, f); }

static void add_line(help_state_t *s, int off, int len, int kind, int cont) {
    if (s->nlines >= MAX_LINES)
        return;
    line_t *l = &s->lines[s->nlines++];
    l->off = (uint16_t)off;
    l->len = (uint16_t)len;
    l->kind = (uint8_t)kind;
    l->cont = (uint8_t)cont;
}

/* one paragraph (text[a..b), its newlines read as spaces) in lines of at
 * most `w` pixels */
static void wrap_para(help_state_t *s, int a, int b, int w, int f, int kind) {
    char *t = s->text;
    int o = a; /* newlines and runs of spaces: one space */
    for (int i = a; i < b; i++) {
        char ch = t[i] == '\n' || t[i] == '\r' ? ' ' : t[i];
        if (ch != ' ' || (o > a && t[o - 1] != ' '))
            t[o++] = ch;
    }
    for (int i = o; i < b; i++)
        t[i] = ' ';
    int first = 1;
    while (a < b) {
        while (a < b && t[a] == ' ')
            a++;
        if (a >= b)
            break;
        int end = a, fit = a; /* the last word end that fits */
        while (end < b) {
            int we = end;
            while (we < b && t[we] == ' ')
                we++;
            while (we < b && t[we] != ' ')
                we++;
            if (text_w(t + a, we - a, f) > w && fit > a)
                break;
            fit = we;
            end = we;
            if (text_w(t + a, we - a, f) > w)
                break; /* one word wider than the page: on its own */
        }
        add_line(s, a, fit - a, kind, !first);
        first = 0;
        a = fit;
    }
}

static void layout(help_state_t *s, int w) {
    s->nlines = 0;
    s->laid_w = w;
    if (!s->raw || !s->text)
        return;
    strcpy(s->text, s->raw); /* (wrap turns a paragraph's newlines into
                                spaces) */
    char *t = s->text;
    int n = (int)strlen(t), i = 0, fs = UI_FONT_SMALL, fb = UI_FONT_BOLD;
    while (i < n) {
        int e = i; /* the line [i, e) */
        while (e < n && t[e] != '\n')
            e++;
        int le = e > i && t[e - 1] == '\r' ? e - 1 : e;
        if (le == i) { /* blank: a gap */
            if (s->nlines && s->lines[s->nlines - 1].kind != L_GAP)
                add_line(s, i, 0, L_GAP, 0);
        } else if (t[i] == '#') {
            int h = i;
            while (h < le && (t[h] == '#' || t[h] == ' '))
                h++;
            if (s->nlines && s->lines[s->nlines - 1].kind != L_GAP)
                add_line(s, i, 0, L_GAP, 0);
            wrap_para(s, h, le, w, fb, L_HEAD);
            add_line(s, i, 0, L_GAP, 0);
        } else if (t[i] == '-' && i + 1 < le && t[i + 1] == ' ') {
            /* a point: its own lines, up to the next point or blank line */
            int pe = e;
            while (pe < n) {
                int ne = pe + 1, nx = ne;
                while (nx < n && t[nx] != '\n')
                    nx++;
                if (ne >= n || nx == ne || t[ne] == '-' || t[ne] == '#' ||
                    (nx == ne + 1 && t[ne] == '\r'))
                    break;
                pe = nx;
            }
            wrap_para(s, i + 2, pe, w - 8, fs, L_BULLET);
            e = pe;
        } else { /* a paragraph: up to a blank line or something else */
            int pe = e;
            while (pe < n) {
                int ne = pe + 1, nx = ne;
                while (nx < n && t[nx] != '\n')
                    nx++;
                if (ne >= n || nx == ne || t[ne] == '#' ||
                    (t[ne] == '-' && ne + 1 < n && t[ne + 1] == ' ') ||
                    (nx == ne + 1 && t[ne] == '\r'))
                    break;
                pe = nx;
            }
            wrap_para(s, i, pe, w, fs, L_TEXT);
            e = pe;
        }
        i = e + 1;
    }
}

static int line_h(const line_t *l) {
    return l->kind == L_GAP    ? 4
           : l->kind == L_HEAD ? font_height(UI_FONT_BOLD) + 2
                               : font_height(UI_FONT_SMALL) + 2;
}

static int content_h(help_state_t *s) {
    int h = 0;
    for (int i = 0; i < s->nlines; i++)
        h += line_h(&s->lines[i]);
    return h;
}

static void update_sb(help_state_t *s) {
    int page = s->view->ah - 4 - 2 * PAD;
    int max = content_h(s) - page;
    ctl_set_range(s->sb, 0, max > 0 ? max : 0, page > 8 ? page - 8 : 1);
    if (s->top > (max > 0 ? max : 0))
        s->top = max > 0 ? max : 0;
    ctl_set_value(s->sb, s->top);
}

static void show(help_state_t *s, const char *rel) {
    char path[FS_PATH_SZ];
    snprintf(path, sizeof(path), "%s/%s", HELP_DIR, rel);
    dir_entry_t de;
    if (fs_find(path, &de) && (de.attr & ATTR_DIRECTORY)) {
        /* a folder: its first page */
        static listing_t in;
        list_sorted(path, &in);
        for (int k = 0; k < in.n; k++)
            if (!in.dir[k]) {
                char r[REL_SZ];
                snprintf(r, sizeof(r), "%s/%s", rel, in.names[k]);
                snprintf(path, sizeof(path), "%s/%s", HELP_DIR, r);
                break;
            }
    }
    snprintf(s->shown, sizeof(s->shown), "%s", rel);
    kfree(s->raw);
    kfree(s->text);
    s->raw = s->text = NULL;
    fat_file_t f;
    if (fs_open(path, &f) && !(f.entry.attr & ATTR_DIRECTORY)) {
        uint32_t n =
            f.entry.file_size < PAGE_MAX ? f.entry.file_size : PAGE_MAX;
        s->raw = kmalloc(n + 1);
        s->text = kmalloc(n + 1);
        if (s->raw && s->text) {
            int got = fs_read(&f, s->raw, (int)n), k = 0;
            for (int i = 0; i < got; i++) /* (CR LF: the CRs go) */
                if (s->raw[i] != '\r')
                    s->raw[k++] = s->raw[i];
            s->raw[k] = 0;
        } else {
            kfree(s->raw);
            kfree(s->text);
            s->raw = s->text = NULL;
        }
        fs_close(&f);
    }
    s->top = 0;
    s->laid_w = 0; /* laid out at the next paint */
    wm_invalidate(s->win);
}

static void paint_view(control *c, int x, int y, int w, int h) {
    help_state_t *s = c->user;
    fill_rect(x, y, w, h, WHITE);
    if (!s->text) {
        const char *t = s->nrel ? "Pick a page on the left."
                                : "There are no help pages in /HELP.";
        draw_string(x + PAD, y + PAD, t, DARK_GRAY, UI_FONT_SMALL);
        return;
    }
    if (s->laid_w != w - 2 * PAD) {
        layout(s, w - 2 * PAD);
        update_sb(s);
    }
    int cy = y + PAD - s->top;
    for (int i = 0; i < s->nlines && cy < y + h; i++) {
        line_t *l = &s->lines[i];
        int lh = line_h(l);
        if (cy + lh > y && l->kind != L_GAP) {
            int f = l->kind == L_HEAD ? UI_FONT_BOLD : UI_FONT_SMALL;
            int lx = x + PAD + (l->kind == L_BULLET ? 8 : 0);
            if (l->kind == L_BULLET && !l->cont)
                fill_rect(x + PAD + 2, cy + font_height(f) / 2 - 1, 2, 2,
                          BLACK);
            char buf[160];
            int n = l->len < 159 ? l->len : 159;
            memcpy(buf, s->text + l->off, (size_t)n);
            buf[n] = 0;
            draw_string(lx, cy, buf, l->kind == L_HEAD ? BLUE : BLACK, f);
        }
        cy += lh;
    }
}

static void scroll_by(help_state_t *s, int d) {
    s->top += d;
    if (s->top < 0)
        s->top = 0;
    update_sb(s);
    wm_invalidate(s->win);
}

static void on_view(control *c, int ev) {
    help_state_t *s = c->user;
    if (ev != EV_KEY)
        return;
    int page = c->ah - 4 - 2 * PAD - 8;
    switch (kb.last_scancode) {
    case UP_ARROW:
        scroll_by(s, -8);
        break;
    case DOWN_ARROW:
        scroll_by(s, 8);
        break;
    case PAGE_UP:
        scroll_by(s, -page);
        break;
    case PAGE_DOWN:
    case SPACE:
        scroll_by(s, page);
        break;
    case HOME:
        scroll_by(s, -s->top);
        break;
    }
}

static void on_sb(control *c, int ev) {
    help_state_t *s = c->user;
    if (ev == EV_CHANGE) {
        s->top = ctl_value(c);
        wm_invalidate(s->win);
    }
}

static void on_tree(control *c, int ev) {
    help_state_t *s = c->user;
    int n = tree_sel(c);
    if ((ev == EV_CHANGE || ev == EV_SUBMIT) && n >= 0)
        show(s, s->rel[tree_data(c, n)]);
}

/* go to a path in /HELP: "/HELP/FileF", "/HELP/FileF/01_X.TXT" */
static void go(help_state_t *s, const char *path) {
    const char *rel = path;
    int dl = (int)strlen(HELP_DIR);
    if (!strncasecmp(path, HELP_DIR, (size_t)dl))
        rel = path[dl] == '/' ? path + dl + 1 : path + dl;
    int n = rel[0] ? node_of(s, rel) : -1;
    if (n < 0 && rel[0]) { /* a page that isn't there: its folder */
        char f[REL_SZ];
        snprintf(f, sizeof(f), "%s", rel);
        char *sl = strchr(f, '/');
        if (sl)
            *sl = 0;
        n = node_of(s, f);
    }
    if (n < 0)
        n = node_of(s, "ASMOS");
    if (n < 0 && s->nfolders)
        n = s->folders[0];
    if (n < 0)
        return;
    tree_expand(s->tree, n, true);
    tree_set_sel(s->tree, n);
    show(s, s->rel[tree_data(s->tree, n)]);
}

// menus

static help_state_t *front(void) { return ui_active_state(); }

static void del_done(void *user, bool yes) {
    help_state_t *s = user;
    if (!yes)
        return;
    char path[FS_PATH_SZ];
    snprintf(path, sizeof(path), "%s/%s", HELP_DIR, s->shown);
    dir_entry_t de;
    bool dir = fs_find(path, &de) && (de.attr & ATTR_DIRECTORY);
    if (dir)
        fs_rm_rf(path);
    else
        fs_delete(path);
    s->sig = fill_tree(s, true);
    s->fs_rev = g_fs_rev;
    char f[REL_SZ];
    snprintf(f, sizeof(f), "%s", s->shown);
    char *sl = strchr(f, '/');
    if (sl)
        *sl = 0;
    char p[FS_PATH_SZ];
    snprintf(p, sizeof(p), "%s/%s", HELP_DIR, f);
    kfree(s->raw);
    kfree(s->text);
    s->raw = s->text = NULL;
    s->shown[0] = 0;
    go(s, dir ? "" : p);
}

static void m_delete(void) {
    help_state_t *s = front();
    int n = s ? tree_sel(s->tree) : -1;
    if (n < 0)
        return;
    snprintf(s->shown, sizeof(s->shown), "%s", s->rel[tree_data(s->tree, n)]);
    bool dir = !strchr(s->shown, '/');
    char m[200];
    snprintf(m, sizeof(m),
             dir ? "Delete all of \"%s\"'s help pages?\n\nInstalling the "
                   "app again puts them back."
                 : "Delete the page \"%s\"?\n\nInstalling the app again "
                   "puts it back.",
             tree_text(s->tree, n));
    dlg_confirm(s->win, "Help", m, del_done, s);
}

static void m_close(void) { os_quit_app_by_desc(&help_app); }

static void m_about(void) {
    dlg_message(NULL, "About Help",
                "Help 1.0\nA folder of pages for each app, in /HELP.\nAn "
                "app's Help > View Help opens its own.\nApps add their "
                "pages the first time they run.");
}

// the app

void help_open(const char *path) {
    snprintf(s_pending, sizeof(s_pending), "%s", path ? path : "");
    s_has_pending = true;
    os_launch_app(&help_app); /* (or to the front, if it is open) */
}

static void help_init(void *state) {
    help_state_t *s = state;
    s->win = app_window(s, "Help", 6, 12, 300, 172, DARK_GRAY, AW_RESIZABLE);
    if (!s->win)
        return;
    s->win->lazy_custom = true;
    window *w = s->win;
    int wx, wy, cw, ch;
    win_client_rect(w, &wx, &wy, &cw, &ch);
    s->tree =
        ctl_tree(w, 2, 2, TREE_W, ch - 4, TS_FOLDERS | CS_STRETCH_H, on_tree);
    s->view = ctl_custom(
        w, TREE_W + 4, 2, cw - TREE_W - 16, ch - 4, paint_view, on_view,
        CS_BORDER | CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);
    s->sb = ctl_vscroll(w, cw - 10, 2, ch - 11, on_sb);
    ctl_style_add(s->sb, CS_ANCHOR_RIGHT | CS_STRETCH_H | CS_NOFOCUS);
    menu *m = window_add_menu(w, "File");
    menu_add_item(m, "Delete Page", m_delete);
    menu_add_separator(m);
    menu_add_item(m, "About Help", m_about);
    menu_add_separator(m);
    menu_add_item(m, "Close", m_close);
    s->sig = fill_tree(s, true);
    s->fs_rev = g_fs_rev;
    if (!s_has_pending)
        go(s, "");
}

static void help_frame(void *state) {
    help_state_t *s = state;
    if (!s->win)
        return;
    if (s->fs_rev != g_fs_rev) { /* pages added or deleted: list again */
        s->fs_rev = g_fs_rev;
        uint32_t sig = fill_tree(s, false);
        if (sig != s->sig) {
            char keep[REL_SZ];
            snprintf(keep, sizeof(keep), "%s", s->shown);
            s->sig = fill_tree(s, true);
            int n = node_of(s, keep);
            if (n >= 0)
                tree_set_sel(s->tree, n);
        }
    }
    if (s_has_pending) {
        s_has_pending = false;
        go(s, s_pending);
        wm_focus(s->win);
    }
}

static void help_destroy(void *state) {
    help_state_t *s = state;
    kfree(s->raw);
    kfree(s->text);
    s->raw = s->text = NULL;
}

app_descriptor help_app = {
    .name = "HELP",
    .state_size = sizeof(help_state_t),
    .init = help_init,
    .on_frame = help_frame,
    .destroy = help_destroy,
    .single_instance = true,
};
