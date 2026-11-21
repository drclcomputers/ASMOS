/*
 * Growable tree nodes store parent/child/sibling indices. Expandable nodes
 * send EV_EXPAND on first open so handlers can add children. Mouse and
 * keyboard navigation update selection; double-click and Enter submit.
 */
#include "drivers/keyboard.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "ui/ctl_internal.h"
#include "ui/uifont.h"

#define ROW_H 9
#define SB_W 8 /* same as the list's built-in scrollbar */

static uint8_t bgc_or(const control *c, uint8_t d) {
    return c->bg != 0xFF ? c->bg : d;
}
static uint8_t fgc_or(const control *c, uint8_t d) {
    return !c->enabled ? TH_DISABLED : c->fg != 0xFF ? c->fg : d;
}
#define INDENT 9

#define N_USED 0x01
#define N_OPEN 0x02
#define N_CAN 0x04   /* may have children (show [+] until known) */
#define N_ASKED 0x08 /* EV_EXPAND already sent */

typedef struct {
    char text[64]; /* a folder's long name (FS_NAME_SZ) */
    uint32_t data;
    int16_t parent, first, next;
    uint8_t depth, flags;
} tnode_t;

typedef struct {
    tnode_t *n;
    int cap, used;
    int *vis; /* visible nodes in display order */
    int nvis, vis_cap;
    bool dirty;
    int event_node;
    int sb_part; /* scrollbar drag state */
} tree_t;

static tree_t *T(const control *c) {
    return (c && c->type == CTL_TREE) ? (tree_t *)c->ext : NULL;
}

// storage

static int alloc_node(tree_t *t) {
    for (int i = 0; i < t->used; i++)
        if (!(t->n[i].flags & N_USED))
            return i;
    if (t->used == t->cap) {
        int nc = t->cap ? t->cap * 2 : 32;
        tnode_t *nn = (tnode_t *)kmalloc(sizeof(tnode_t) * nc);
        if (!nn)
            return -1;
        if (t->n) {
            memcpy(nn, t->n, sizeof(tnode_t) * t->cap);
            kfree(t->n);
        }
        t->n = nn;
        t->cap = nc;
    }
    return t->used++;
}

static bool valid(const tree_t *t, int i) {
    return t && i >= 0 && i < t->used && (t->n[i].flags & N_USED);
}

/* Rebuild the list of visible rows (depth-first through open nodes). */
static void rebuild(tree_t *t) {
    if (!t->dirty)
        return;
    t->dirty = false;
    t->nvis = 0;
    if (t->vis_cap < t->used) {
        if (t->vis)
            kfree(t->vis);
        t->vis_cap = t->used + 16;
        t->vis = (int *)kmalloc(sizeof(int) * t->vis_cap);
        if (!t->vis) {
            t->vis_cap = 0;
            return;
        }
    }
    /* iterative DFS over roots (parent -1) in creation order */
    for (int r = 0; r < t->used; r++) {
        if (!(t->n[r].flags & N_USED) || t->n[r].parent != -1)
            continue;
        int i = r;
        while (i >= 0) {
            t->vis[t->nvis++] = i;
            if ((t->n[i].flags & N_OPEN) && t->n[i].first >= 0) {
                i = t->n[i].first;
                continue;
            }
            /* next sibling, or climb until one exists (stop at the root) */
            while (i >= 0 && i != r && t->n[i].next < 0)
                i = t->n[i].parent;
            i = (i < 0 || i == r) ? -1 : t->n[i].next;
        }
    }
}

static int rows_of(const control *c) {
    int r = (c->ah - 4) / ROW_H;
    return r < 1 ? 1 : r;
}

static int vis_index(tree_t *t, int node) {
    rebuild(t);
    for (int k = 0; k < t->nvis; k++)
        if (t->vis[k] == node)
            return k;
    return -1;
}

static void ensure_visible(control *c) {
    tree_t *t = T(c);
    rebuild(t);
    int rows = rows_of(c), k = vis_index(t, c->sel);
    if (k >= 0) {
        if (k < c->top)
            c->top = k;
        if (k >= c->top + rows)
            c->top = k - rows + 1;
    }
    if (c->top > t->nvis - rows)
        c->top = t->nvis - rows;
    if (c->top < 0)
        c->top = 0;
}

// public API

control *ctl_tree(window *w, int x, int y, int cw, int ch, uint32_t style,
                  ctl_handler h) {
    control *c = ctl_create(w, CTL_TREE, x, y, cw, ch, "", style, h);
    if (!c)
        return NULL;
    tree_t *t = (tree_t *)kmalloc(sizeof(tree_t));
    if (!t) {
        ctl_destroy(c);
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->event_node = -1;
    c->ext = t;
    c->sel = -1;
    return c;
}

void tree_free(control *c) {
    tree_t *t = T(c);
    if (!t)
        return;
    if (t->n)
        kfree(t->n);
    if (t->vis)
        kfree(t->vis);
    kfree(t);
    c->ext = NULL;
}

int tree_add(control *c, int parent, const char *text, uint32_t data,
             bool can_expand) {
    tree_t *t = T(c);
    if (!t || (parent != -1 && !valid(t, parent)))
        return -1;
    int i = alloc_node(t);
    if (i < 0)
        return -1;
    tnode_t *n = &t->n[i];
    memset(n, 0, sizeof(*n));
    strncpy(n->text, text ? text : "", sizeof(n->text) - 1);
    n->data = data;
    n->parent = (int16_t)parent;
    n->first = n->next = -1;
    n->flags = N_USED | (can_expand ? N_CAN : 0);
    if (parent >= 0) {
        tnode_t *p = &t->n[parent];
        n->depth = (uint8_t)(p->depth + 1);
        p->flags |= N_CAN;
        if (p->first < 0)
            p->first = (int16_t)i;
        else {
            int k = p->first;
            while (t->n[k].next >= 0)
                k = t->n[k].next;
            t->n[k].next = (int16_t)i;
        }
    }
    t->dirty = true;
    return i;
}

static void drop_subtree(tree_t *t, int i) {
    for (int k = t->n[i].first; k >= 0;) {
        int nx = t->n[k].next;
        drop_subtree(t, k);
        t->n[k].flags = 0;
        k = nx;
    }
    t->n[i].first = -1;
}

void tree_clear_children(control *c, int node) {
    tree_t *t = T(c);
    if (!valid(t, node))
        return;
    int sel = c->sel;
    drop_subtree(t, node);
    t->n[node].flags &= (uint8_t)~N_ASKED; /* reload on next open */
    if (sel >= 0 && !valid(t, sel))
        c->sel = node;
    t->dirty = true;
    ensure_visible(c);
}

void tree_clear(control *c) {
    tree_t *t = T(c);
    if (!t)
        return;
    t->used = 0;
    t->dirty = true;
    c->sel = -1;
    c->top = 0;
}

int tree_parent(const control *c, int node) {
    tree_t *t = T(c);
    return valid(t, node) ? t->n[node].parent : -1;
}
int tree_first_child(const control *c, int node) {
    tree_t *t = T(c);
    return valid(t, node) ? t->n[node].first : -1;
}
int tree_next_sibling(const control *c, int node) {
    tree_t *t = T(c);
    return valid(t, node) ? t->n[node].next : -1;
}
const char *tree_text(const control *c, int node) {
    tree_t *t = T(c);
    return valid(t, node) ? t->n[node].text : "";
}
uint32_t tree_data(const control *c, int node) {
    tree_t *t = T(c);
    return valid(t, node) ? t->n[node].data : 0;
}
int tree_sel(const control *c) { return T(c) ? c->sel : -1; }
int tree_event_node(const control *c) { return T(c) ? T(c)->event_node : -1; }
bool tree_is_expanded(const control *c, int node) {
    tree_t *t = T(c);
    return valid(t, node) && (t->n[node].flags & N_OPEN);
}

static void select_node(control *c, int node);

void tree_expand(control *c, int node, bool open) {
    tree_t *t = T(c);
    if (!valid(t, node))
        return;
    tnode_t *n = &t->n[node];
    if (open) {
        if (!(n->flags & N_CAN))
            return;
        if (!(n->flags & N_ASKED)) {
            n->flags |= N_ASKED;
            t->event_node = node;
            ctl_invoke(c, EV_EXPAND); /* may add children */
            t = T(c);
            if (!t || !valid(t, node))
                return;
            n = &t->n[node];
        }
        if (n->first < 0) {
            n->flags &= (uint8_t)~N_CAN; /* nothing inside after all */
            return;
        }
        n->flags |= N_OPEN;
    } else {
        n->flags &= (uint8_t)~N_OPEN;
        t->dirty = true;
        /* selection inside the closed branch moves to the branch */
        for (int k = c->sel; k >= 0; k = t->n[k].parent)
            if (k == node && c->sel != node) {
                select_node(c, node); /* EV_CHANGE */
                return;
            }
    }
    t->dirty = true;
    ensure_visible(c);
}

void tree_set_sel(control *c, int node) {
    tree_t *t = T(c);
    if (!t)
        return;
    if (node >= 0 && !valid(t, node))
        node = -1;
    for (int p = node >= 0 ? t->n[node].parent : -1; p >= 0; p = t->n[p].parent)
        if (!(t->n[p].flags & N_OPEN)) {
            t->n[p].flags |= N_OPEN | N_ASKED; /* children already exist */
            t->dirty = true;
        }
    c->sel = node;
    ensure_visible(c);
}

void tree_set_text(control *c, int node, const char *text) {
    tree_t *t = T(c);
    if (valid(t, node))
        strncpy(t->n[node].text, text, sizeof(t->n[node].text) - 1);
}

// drawing

// path helpers (node texts as path components)

void tree_path(const control *c, int node, char *out, int sz) {
    const char *parts[48];
    int n = 0;
    for (int k = node; k >= 0 && n < 48; k = tree_parent(c, k))
        parts[n++] = tree_text(c, k);
    out[0] = 0;
    int len = 0;
    while (n-- > 0 && len < sz - 1)
        len += snprintf(out + len, sz - len, "/%s", parts[n]);
}

static int child_named(tree_t *t, int node, const char *name) {
    if (node < 0) {
        for (int k = 0; k < t->used; k++)
            if ((t->n[k].flags & N_USED) && t->n[k].parent == -1 &&
                !strcasecmp(t->n[k].text, name))
                return k;
        return -1;
    }
    for (int k = t->n[node].first; k >= 0; k = t->n[k].next)
        if (!strcasecmp(t->n[k].text, name))
            return k;
    return -1;
}

int tree_find_path(control *c, const char *path) {
    tree_t *t = T(c);
    if (!t)
        return -1;
    char comp[64];
    int node = -1;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        int n = 0;
        while (*p && *p != '/') {
            if (n < (int)sizeof(comp) - 1)
                comp[n++] = *p;
            p++;
        }
        comp[n] = 0;
        if (node >= 0)
            tree_expand(c, node, true);
        int k = child_named(t, node, comp);
        if (k < 0 && node >= 0) { /* created since loaded: ask again */
            tree_clear_children(c, node);
            tree_expand(c, node, true);
            k = child_named(t, node, comp);
        }
        if (k < 0)
            return node;
        node = k;
    }
    return node;
}

static void draw_expander(int x, int y, bool open) {
    fill_rect(x, y, 7, 7, TH_FIELD);
    draw_rect(x, y, 7, 7, TH_SHADOW);
    draw_line(x + 2, y + 3, x + 4, y + 3, BLACK);
    if (!open)
        draw_line(x + 3, y + 2, x + 3, y + 4, BLACK);
}

static void draw_folder(int x, int y, bool open) {
    fill_rect(x, y + 1, 3, 1, open ? YELLOW : BROWN);
    fill_rect(x, y + 2, 8, 5, open ? YELLOW : BROWN);
    draw_rect(x, y + 2, 8, 5, BLACK);
}

/* checksum of what tree_draw would show */
uint32_t tree_sig(const control *c) {
    tree_t *t = T(c);
    if (!t)
        return 0;
    rebuild(t);
    uint32_t h = (uint32_t)t->nvis * 33u + (uint32_t)t->used;
    int rows = rows_of(c);
    for (int r = 0; r < rows && c->top + r < t->nvis; r++) {
        int i = t->vis[c->top + r];
        const tnode_t *n = &t->n[i];
        h = h * 33u ^ (uint32_t)i;
        h = h * 33u ^ ((uint32_t)n->depth << 8 | n->flags);
        for (const char *p = n->text; *p; p++)
            h = h * 33u ^ (uint8_t)*p;
    }
    return h;
}

void tree_draw(control *c) {
    tree_t *t = T(c);
    th_sunken(c->ax, c->ay, c->aw, c->ah, bgc_or(c, TH_FIELD));
    if (!t)
        return;
    rebuild(t);
    int rows = rows_of(c);
    bool sb = t->nvis > rows;
    int iw = c->aw - 4 - (sb ? SB_W : 0);
    bool icons = (c->style & TS_FOLDERS) != 0;
    clip_push(c->ax + 2, c->ay + 2, iw, c->ah - 4);
    for (int r = 0; r < rows && c->top + r < t->nvis; r++) {
        int i = t->vis[c->top + r];
        tnode_t *n = &t->n[i];
        int y = c->ay + 2 + r * ROW_H;
        int x = c->ax + 3 + n->depth * INDENT;
        if (n->flags & N_CAN)
            draw_expander(x, y + 1, (n->flags & N_OPEN) != 0);
        x += INDENT;
        if (icons) {
            draw_folder(x, y, (n->flags & N_OPEN) != 0);
            x += 11;
        }
        uint8_t col = fgc_or(c, TH_TEXT);
        if (i == c->sel) {
            fill_rect(x - 1, y, text_width(n->text, UI_FONT_SMALL) + 2, ROW_H,
                      c->focused ? TH_SEL_BG : TH_SHADOW);
            col = TH_SEL_FG;
        }
        draw_string(x + 1, y + 2, n->text, col, UI_FONT_SMALL);
    }
    clip_pop();
    if (sb)
        sb_draw(c->ax + c->aw - 2 - SB_W, c->ay + 2, c->ah - 4, true, c->top,
                t->nvis - rows, rows);
    if (c->focused && c->sel < 0)
        th_focus(c->ax + 2, c->ay + 2, iw, c->ah - 4);
}

// input

static void select_node(control *c, int node) {
    if (node == c->sel)
        return;
    c->sel = node;
    ensure_visible(c);
    ctl_invoke(c, EV_CHANGE);
}

void tree_mouse(control *c, int mx, int my, bool down, bool dbl) {
    tree_t *t = T(c);
    if (!t)
        return;
    rebuild(t);
    int rows = rows_of(c);
    bool sb = t->nvis > rows;
    if (!down) { /* dragging the scrollbar thumb */
        if (t->sb_part == 5) {
            int part = 5;
            c->top = sb_mouse(my - 2, c->ah - 4, c->top, t->nvis - rows, rows,
                              false, &part);
            ensure_visible(c);
        }
        return;
    }
    t->sb_part = 0;
    if (sb && mx >= c->aw - 2 - SB_W) {
        c->top = sb_mouse(my - 2, c->ah - 4, c->top, t->nvis - rows, rows, true,
                          &t->sb_part);
        ensure_visible(c);
        return;
    }
    int k = c->top + (my - 2) / ROW_H;
    if (my < 2 || k >= t->nvis)
        return;
    int i = t->vis[k];
    tnode_t *n = &t->n[i];
    int ex = 3 + n->depth * INDENT;
    if ((n->flags & N_CAN) && mx >= ex - 1 && mx < ex + 9) {
        tree_expand(c, i, !(n->flags & N_OPEN));
        return;
    }
    select_node(c, i);
    if (dbl && T(c) && valid(T(c), i)) {
        if (T(c)->n[i].flags & N_CAN)
            tree_expand(c, i, !(T(c)->n[i].flags & N_OPEN));
        ctl_invoke(c, EV_SUBMIT);
    }
}

bool tree_key(control *c, uint8_t sc, char ch) {
    tree_t *t = T(c);
    if (!t)
        return false;
    rebuild(t);
    if (!t->nvis)
        return false;
    int k = vis_index(t, c->sel), rows = rows_of(c);
    int nk = k;
    switch (sc) {
    case UP_ARROW:
        nk = k - 1;
        break;
    case DOWN_ARROW:
        nk = k + 1;
        break;
    case PAGE_UP:
        nk = k - rows;
        break;
    case PAGE_DOWN:
        nk = k + rows;
        break;
    case HOME:
        nk = 0;
        break;
    case END:
        nk = t->nvis - 1;
        break;
    case RIGHT_ARROW:
        if (c->sel < 0)
            nk = 0;
        else if (!(t->n[c->sel].flags & N_OPEN)) {
            tree_expand(c, c->sel, true);
            return true;
        } else if (t->n[c->sel].first >= 0) {
            select_node(c, t->n[c->sel].first);
            return true;
        }
        break;
    case LEFT_ARROW:
        if (c->sel < 0)
            return true;
        if (t->n[c->sel].flags & N_OPEN)
            tree_expand(c, c->sel, false);
        else if (t->n[c->sel].parent >= 0)
            select_node(c, t->n[c->sel].parent);
        return true;
    case ENTER:
        if (c->sel < 0)
            return false;
        ctl_invoke(c, EV_SUBMIT);
        return true;
    default:
        if (ch == '+' || ch == '-') {
            if (c->sel >= 0)
                tree_expand(c, c->sel, ch == '+');
            return true;
        }
        if (ch > ' ' && ch < 127) { /* next node starting with ch */
            char lc = ch | 0x20;
            for (int d = 1; d <= t->nvis; d++) {
                int j = t->vis[(k + d + t->nvis) % t->nvis];
                if ((t->n[j].text[0] | 0x20) == lc) {
                    select_node(c, j);
                    return true;
                }
            }
            return true;
        }
        return false;
    }
    if (nk < 0)
        nk = 0;
    if (nk >= t->nvis)
        nk = t->nvis - 1;
    select_node(c, t->vis[nk]);
    return true;
}

/* Right-click: select the row under the point (no expand/collapse). */
void tree_select_at(control *c, int mx, int my) {
    tree_t *t = T(c);
    if (!t)
        return;
    rebuild(t);
    if (t->nvis > rows_of(c) && mx >= c->aw - 2 - SB_W)
        return;
    int k = c->top + (my - 2) / ROW_H;
    if (my >= 2 && k < t->nvis)
        select_node(c, t->vis[k]);
}

/* Where a keyboard-opened context menu should appear: by the selection. */
void tree_sel_point(const control *c, int *mx, int *my) {
    tree_t *t = T(c);
    int k = t && c->sel >= 0 ? vis_index(t, c->sel) : -1;
    if (k < 0) {
        *mx = 8;
        *my = 8;
        return;
    }
    *mx = 3 + t->n[c->sel].depth * INDENT + 20;
    *my = 2 + (k - c->top) * ROW_H + ROW_H / 2;
}
