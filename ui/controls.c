/* controls.c - ASMOS UI toolkit: control objects, drawing, input routing.
 * Edit controls live in ctl_edit.c; dialogs in dialogs.c. */
#include "ui/controls.h"
#include "ui/ctl_internal.h"

#include "config/config.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "os/os.h"
#include "ui/cursor.h"
#include "ui/menubar.h"
#include "ui/modal.h"
#include "ui/window.h"

void (*g_ctl_invoke_hook)(control *c, int ev) = NULL;

static control *s_capture;   /* control that owns the mouse while held */
static control *s_popup;     /* combo box whose list is open           */
static control *s_last_down; /* for double clicks                       */
static uint32_t s_last_down_t;
static int s_last_down_item = -1;
static int s_drag_off; /* scrollbar thumb grab offset */
static int s_drag_part;

#define DBLCLICK_MS 450
#define SB_W 8 /* built-in scrollbar width */
#define POPUP_ROWS 8

/* ── small helpers ─────────────────────────────────────────────────────── */

void win_client_rect(const window *w, int *x, int *y, int *cw, int *ch) {
    *x = w->x + 1;
    *y = w->y + MENUBAR_H + TITLEBAR_H;
    *cw = w->w - 2;
    *ch = w->h - TITLEBAR_H - 1;
}

void ctl_layout(control *c) {
    int cx, cy, cw, ch;
    win_client_rect(c->win, &cx, &cy, &cw, &ch);
    int rm = c->base_cw - (c->x + c->w); /* right margin at creation  */
    int bm = c->base_ch - (c->y + c->h); /* bottom margin at creation */
    c->aw = c->w;
    c->ah = c->h;
    c->ax = cx + c->x;
    c->ay = cy + c->y;
    if (c->style & CS_STRETCH_W)
        c->aw = cw - c->x - rm;
    else if (c->style & CS_ANCHOR_RIGHT)
        c->ax = cx + cw - rm - c->w;
    if (c->style & CS_STRETCH_H)
        c->ah = ch - c->y - bm;
    else if (c->style & CS_ANCHOR_BOTTOM)
        c->ay = cy + ch - bm - c->h;
    if (c->aw < 1)
        c->aw = 1;
    if (c->ah < 1)
        c->ah = 1;
}

bool ctl_shown(const control *c) {
    while (c) {
        if (!c->visible)
            return false;
        if (!c->tab_owner)
            return true;
        if (c->tab_owner->value != c->tab_page)
            return false;
        c = c->tab_owner;
    }
    return true;
}

static bool focusable(const control *c) {
    if (!ctl_shown(c) || !c->enabled || (c->style & CS_NOFOCUS))
        return false;
    switch (c->type) {
    case CTL_LABEL:
    case CTL_GROUP:
    case CTL_PROGRESS:
        return false;
    case CTL_CUSTOM:
        return (c->style & CS_FOCUSABLE) != 0;
    default:
        return true;
    }
}

static bool inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

void ctl_invoke(control *c, int ev) {
    if (!c || !c->handler)
        return;
    if (g_ctl_invoke_hook && c->win && c->win->dyn_state)
        g_ctl_invoke_hook(c, ev);
    else
        c->handler(c, ev);
}

static void set_text_raw(control *c, const char *t) {
    int n = t ? (int)strlen(t) : 0;
    if (c->max_len > 0 && n > c->max_len)
        n = c->max_len;
    if (n + 1 > c->text_cap) {
        int cap = n + 16;
        char *nb = (char *)kmalloc(cap);
        if (!nb)
            return;
        if (c->text)
            kfree(c->text);
        c->text = nb;
        c->text_cap = cap;
    }
    if (n)
        memcpy(c->text, t, n);
    c->text[n] = 0;
    c->text_len = n;
}

bool ctl_text_reserve(control *c, int need) {
    if (need + 1 <= c->text_cap)
        return true;
    int cap = c->text_cap ? c->text_cap : 16;
    while (cap < need + 1)
        cap *= 2;
    char *nb = (char *)kmalloc(cap);
    if (!nb)
        return false;
    if (c->text)
        memcpy(nb, c->text, c->text_len + 1);
    else
        nb[0] = 0;
    if (c->text)
        kfree(c->text);
    c->text = nb;
    c->text_cap = cap;
    return true;
}

/* ── drawing primitives ────────────────────────────────────────────────── */

static int cl_x, cl_y, cl_w, cl_h;
void clip_push(int x, int y, int w, int h) {
    gfx_get_clip(&cl_x, &cl_y, &cl_w, &cl_h);
    int x0 = x > cl_x ? x : cl_x, y0 = y > cl_y ? y : cl_y;
    int x1 = (x + w < cl_x + cl_w) ? x + w : cl_x + cl_w;
    int y1 = (y + h < cl_y + cl_h) ? y + h : cl_y + cl_h;
    gfx_set_clip(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}
void clip_pop(void) { gfx_set_clip(cl_x, cl_y, cl_w, cl_h); }

void th_raised(int x, int y, int w, int h, uint8_t face) {
    fill_rect(x, y, w, h, face);
    draw_rect(x, y, w, h, BLACK);
    draw_line(x + 1, y + 1, x + w - 2, y + 1, TH_LIGHT);
    draw_line(x + 1, y + 1, x + 1, y + h - 2, TH_LIGHT);
    draw_line(x + 1, y + h - 2, x + w - 2, y + h - 2, TH_SHADOW);
    draw_line(x + w - 2, y + 1, x + w - 2, y + h - 2, TH_SHADOW);
}

void th_pressed(int x, int y, int w, int h, uint8_t face) {
    fill_rect(x, y, w, h, face);
    draw_rect(x, y, w, h, BLACK);
    draw_line(x + 1, y + 1, x + w - 2, y + 1, TH_SHADOW);
    draw_line(x + 1, y + 1, x + 1, y + h - 2, TH_SHADOW);
}

void th_sunken(int x, int y, int w, int h, uint8_t bg) {
    fill_rect(x, y, w, h, bg);
    draw_line(x, y, x + w - 1, y, TH_SHADOW);
    draw_line(x, y, x, y + h - 1, TH_SHADOW);
    draw_line(x, y + h - 1, x + w - 1, y + h - 1, TH_LIGHT);
    draw_line(x + w - 1, y, x + w - 1, y + h - 1, TH_LIGHT);
    draw_line(x + 1, y + 1, x + w - 2, y + 1, BLACK);
    draw_line(x + 1, y + 1, x + 1, y + h - 2, BLACK);
}

void th_focus(int x, int y, int w, int h) {
    for (int i = x; i < x + w; i += 2) {
        draw_dot(i, y, BLACK);
        draw_dot(i, y + h - 1, BLACK);
    }
    for (int j = y; j < y + h; j += 2) {
        draw_dot(x, j, BLACK);
        draw_dot(x + w - 1, j, BLACK);
    }
}

static void arrow(int cx, int cy, int dir, uint8_t col) {
    /* dir: 0 up, 1 down, 2 left, 3 right; 3 rows tall */
    for (int i = 0; i < 3; i++) {
        switch (dir) {
        case 0:
            draw_line(cx - i, cy - 1 + i, cx + i, cy - 1 + i, col);
            break;
        case 1:
            draw_line(cx - i, cy + 1 - i, cx + i, cy + 1 - i, col);
            break;
        case 2:
            draw_line(cx - 1 + i, cy - i, cx - 1 + i, cy + i, col);
            break;
        default:
            draw_line(cx + 1 - i, cy - i, cx + 1 - i, cy + i, col);
            break;
        }
    }
}

static uint8_t fgc(const control *c, uint8_t def) {
    if (!c->enabled)
        return TH_DISABLED;
    return c->fg != 0xFF ? c->fg : def;
}
static uint8_t bgc(const control *c, uint8_t def) {
    return c->bg != 0xFF ? c->bg : def;
}

/* Text inside a box, vertically centred, clipped by the current clip. */
static void text_in(int x, int y, int w, int h, const char *t, uint8_t col,
                    int align) {
    int tw = (int)strlen(t) * TH_CHAR_W - 1;
    int tx = x;
    if (align == 1)
        tx = x + (w - tw) / 2;
    else if (align == 2)
        tx = x + w - tw;
    draw_string(tx, y + (h - 6) / 2, t, col, 2);
}

/* ── scrollbar geometry (shared by lists, edits, standalone bars) ──────── */

typedef struct {
    int pos, len; /* thumb along the track */
    int track0, track_len;
} sb_geo_t;

static void sb_geo(int len, int value, int max, int page, sb_geo_t *g) {
    g->track0 = SB_W;
    g->track_len = len - 2 * SB_W;
    if (g->track_len < 4)
        g->track_len = 4;
    int total = max + page;
    if (total <= 0 || max <= 0) {
        g->len = g->track_len;
        g->pos = g->track0;
        return;
    }
    g->len = g->track_len * page / total;
    if (g->len < 6)
        g->len = 6;
    if (g->len > g->track_len)
        g->len = g->track_len;
    g->pos = g->track0 + (g->track_len - g->len) * value / max;
}

void sb_draw(int x, int y, int len, bool vert, int value, int max, int page) {
    sb_geo_t g;
    sb_geo(len, value, max, page, &g);
    int w = vert ? SB_W : len, h = vert ? len : SB_W;
    fill_rect(x, y, w, h, TH_SHADOW);
    if (vert) {
        th_raised(x, y, SB_W, SB_W, TH_FACE);
        arrow(x + SB_W / 2 - 1, y + SB_W / 2 - 1, 0, BLACK);
        th_raised(x, y + len - SB_W, SB_W, SB_W, TH_FACE);
        arrow(x + SB_W / 2 - 1, y + len - SB_W / 2 - 1, 1, BLACK);
        if (max > 0)
            th_raised(x, y + g.pos, SB_W, g.len, TH_FACE);
    } else {
        th_raised(x, y, SB_W, SB_W, TH_FACE);
        arrow(x + SB_W / 2 - 1, y + SB_W / 2 - 1, 2, BLACK);
        th_raised(x + len - SB_W, y, SB_W, SB_W, TH_FACE);
        arrow(x + len - SB_W / 2 - 1, y + SB_W / 2 - 1, 3, BLACK);
        if (max > 0)
            th_raised(x + g.pos, y, g.len, SB_W, TH_FACE);
    }
}

/* Mouse on a scrollbar. part: 1 arrow-, 2 arrow+, 3 track-, 4 track+,
 * 5 thumb. Returns the new value. */
int sb_mouse(int along, int len, int value, int max, int page, bool down,
             int *part) {
    sb_geo_t g;
    sb_geo(len, value, max, page, &g);
    if (down) {
        if (along < SB_W)
            *part = 1;
        else if (along >= len - SB_W)
            *part = 2;
        else if (along < g.pos)
            *part = 3;
        else if (along >= g.pos + g.len)
            *part = 4;
        else {
            *part = 5;
            s_drag_off = along - g.pos;
        }
    }
    int v = value;
    switch (*part) {
    case 1:
        if (down)
            v -= 1;
        break;
    case 2:
        if (down)
            v += 1;
        break;
    case 3:
        if (down)
            v -= page > 1 ? page - 1 : 1;
        break;
    case 4:
        if (down)
            v += page > 1 ? page - 1 : 1;
        break;
    case 5: {
        int span = g.track_len - g.len;
        if (span > 0)
            v = (along - s_drag_off - g.track0) * max / span;
        break;
    }
    }
    if (v > max)
        v = max;
    if (v < 0)
        v = 0;
    return v;
}

/* ── creation / destruction ────────────────────────────────────────────── */

control *ctl_create(window *w, ctl_type type, int x, int y, int cw, int ch,
                    const char *text, uint32_t style, ctl_handler h) {
    if (!w)
        return NULL;
    control *c = (control *)kzalloc(sizeof(control));
    if (!c)
        return NULL;
    c->win = w;
    c->type = type;
    c->x = x;
    c->y = y;
    c->w = cw;
    c->h = ch;
    c->style = style;
    c->visible = true;
    c->enabled = true;
    c->fg = c->bg = 0xFF;
    c->handler = h;
    c->user = w->userdata;
    c->sel = -1;
    int wx, wy;
    win_client_rect(w, &wx, &wy, &c->base_cw, &c->base_ch);
    set_text_raw(c, text ? text : "");
    /* append (creation order = tab order = paint order) */
    control **pp = &w->controls;
    while (*pp)
        pp = &(*pp)->next;
    *pp = c;
    ctl_layout(c);
    return c;
}

control *ctl_label(window *w, int x, int y, int cw, int ch, const char *text,
                   uint32_t style) {
    return ctl_create(w, CTL_LABEL, x, y, cw, ch, text, style, NULL);
}

control *ctl_button(window *w, int x, int y, int cw, int ch, const char *text,
                    int id, ctl_handler h) {
    control *c = ctl_create(w, CTL_BUTTON, x, y, cw, ch, text, 0, h);
    if (c)
        c->id = id;
    return c;
}

control *ctl_check(window *w, int x, int y, int cw, const char *text,
                   bool checked, ctl_handler h) {
    control *c = ctl_create(w, CTL_CHECK, x, y, cw, 10, text, 0, h);
    if (c)
        c->value = checked;
    return c;
}

control *ctl_radio(window *w, int x, int y, int cw, const char *text, int group,
                   bool selected, ctl_handler h) {
    control *c = ctl_create(w, CTL_RADIO, x, y, cw, 10, text, 0, h);
    if (c) {
        c->group = group;
        c->value = selected;
    }
    return c;
}

control *ctl_list(window *w, int x, int y, int cw, int ch, uint32_t style,
                  ctl_handler h) {
    return ctl_create(w, CTL_LIST, x, y, cw, ch, "", style, h);
}

control *ctl_combo(window *w, int x, int y, int cw, ctl_handler h) {
    return ctl_create(w, CTL_COMBO, x, y, cw, 12, "", 0, h);
}

control *ctl_vscroll(window *w, int x, int y, int ch, ctl_handler h) {
    control *c = ctl_create(w, CTL_VSCROLL, x, y, SB_W, ch, "", 0, h);
    if (c)
        c->page = 1;
    return c;
}

control *ctl_hscroll(window *w, int x, int y, int cw, ctl_handler h) {
    control *c = ctl_create(w, CTL_HSCROLL, x, y, cw, SB_W, "", 0, h);
    if (c)
        c->page = 1;
    return c;
}

control *ctl_slider(window *w, int x, int y, int cw, int min, int max,
                    int value, ctl_handler h) {
    control *c = ctl_create(w, CTL_SLIDER, x, y, cw, 11, "", 0, h);
    if (c) {
        c->min = min;
        c->max = max;
        c->value = value;
    }
    return c;
}

control *ctl_progress(window *w, int x, int y, int cw, int ch, int max) {
    control *c = ctl_create(w, CTL_PROGRESS, x, y, cw, ch, "", 0, NULL);
    if (c)
        c->max = max > 0 ? max : 100;
    return c;
}

control *ctl_group(window *w, int x, int y, int cw, int ch, const char *title) {
    return ctl_create(w, CTL_GROUP, x, y, cw, ch, title, 0, NULL);
}

control *ctl_tabs(window *w, int x, int y, int cw, const char *pages,
                  ctl_handler h) {
    control *c = ctl_create(w, CTL_TABS, x, y, cw, 13, pages, 0, h);
    if (c) {
        c->count = 1;
        for (const char *p = pages; *p; p++)
            if (*p == '|')
                c->count++;
    }
    return c;
}

control *ctl_custom(window *w, int x, int y, int cw, int ch, ctl_painter p,
                    ctl_handler h, uint32_t style) {
    control *c = ctl_create(w, CTL_CUSTOM, x, y, cw, ch, "", style, h);
    if (c)
        c->paint = p;
    return c;
}

static void free_items(control *c) {
    for (int i = 0; i < c->count && c->items; i++)
        kfree(c->items[i]);
    if (c->items)
        kfree(c->items);
    if (c->item_data)
        kfree(c->item_data);
    c->items = NULL;
    c->item_data = NULL;
    c->count = c->cap = 0;
}

static void forget(control *c) {
    if (s_capture == c)
        s_capture = NULL;
    if (s_popup == c)
        s_popup = NULL;
    if (s_last_down == c)
        s_last_down = NULL;
    if (c->win && c->win->focus == c)
        c->win->focus = NULL;
}

void ctl_destroy(control *c) {
    if (!c)
        return;
    window *w = c->win;
    for (control **pp = &w->controls; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    /* controls placed on this tab strip become page-less */
    for (control *o = w->controls; o; o = o->next)
        if (o->tab_owner == c)
            o->tab_owner = NULL;
    forget(c);
    if (c->type != CTL_TABS)
        free_items(c);
    if (c->text)
        kfree(c->text);
    kfree(c);
}

void ctl_window_destroyed(window *w) {
    control *c = w->controls;
    while (c) {
        control *n = c->next;
        forget(c);
        if (c->type != CTL_TABS)
            free_items(c);
        if (c->text)
            kfree(c->text);
        kfree(c);
        c = n;
    }
    w->controls = NULL;
    w->focus = NULL;
}

/* ── setters / getters ─────────────────────────────────────────────────── */

void ctl_set_text(control *c, const char *text) {
    if (!c)
        return;
    set_text_raw(c, text);
    if (c->type == CTL_EDIT) {
        /* single-line: caret at the end; multi-line: at the top */
        c->cursor = c->anchor = (c->style & ES_MULTILINE) ? 0 : c->text_len;
        c->scroll_x = c->scroll_y = 0;
        edit_fix_scroll(c);
    }
    if (c->type == CTL_TABS) {
        c->count = 1;
        for (const char *p = c->text; *p; p++)
            if (*p == '|')
                c->count++;
    }
}

const char *ctl_text(const control *c) { return (c && c->text) ? c->text : ""; }

static void clamp_value(control *c) {
    switch (c->type) {
    case CTL_SLIDER:
        if (c->value < c->min)
            c->value = c->min;
        if (c->value > c->max)
            c->value = c->max;
        break;
    case CTL_VSCROLL:
    case CTL_HSCROLL:
        if (c->value > c->max)
            c->value = c->max;
        if (c->value < 0)
            c->value = 0;
        break;
    case CTL_PROGRESS:
        if (c->value < 0)
            c->value = 0;
        if (c->value > c->max)
            c->value = c->max;
        break;
    case CTL_TABS:
        if (c->value >= c->count)
            c->value = c->count - 1;
        if (c->value < 0)
            c->value = 0;
        break;
    default:
        break;
    }
}

static void radio_pick(control *c) {
    for (control *o = c->win->controls; o; o = o->next)
        if (o->type == CTL_RADIO && o->group == c->group)
            o->value = (o == c);
}

void ctl_set_value(control *c, int v) {
    if (!c)
        return;
    if (c->type == CTL_LIST || c->type == CTL_COMBO) {
        list_set_sel(c, v);
        return;
    }
    c->value = v;
    if (c->type == CTL_RADIO && v)
        radio_pick(c);
    clamp_value(c);
}

int ctl_value(const control *c) {
    if (!c)
        return 0;
    if (c->type == CTL_LIST || c->type == CTL_COMBO)
        return c->sel;
    return c->value;
}

void ctl_set_range(control *c, int min, int max, int page) {
    if (!c)
        return;
    c->min = min;
    c->max = max > 0 || c->type == CTL_SLIDER ? max : 0;
    if (page > 0)
        c->page = page;
    clamp_value(c);
}

void ctl_show(control *c, bool visible) {
    if (!c)
        return;
    c->visible = visible;
    if (!visible && c->win->focus == c)
        c->win->focus = NULL;
    if (!visible && s_popup == c)
        s_popup = NULL;
}

void ctl_enable(control *c, bool enabled) {
    if (!c)
        return;
    c->enabled = enabled;
    if (!enabled && c->win->focus == c)
        c->win->focus = NULL;
}

static void set_focus(window *w, control *c) {
    control *old = w->focus;
    if (old == c)
        return;
    w->focus = c;
    /* focus events only go to custom controls: plain handlers usually
     * don't look at `ev` (a button's handler would "click" on focus). */
    if (old) {
        old->focused = false;
        if (old->type == CTL_CUSTOM)
            ctl_invoke(old, EV_BLUR);
    }
    if (c) {
        c->focused = true;
        if (c->type == CTL_CUSTOM)
            ctl_invoke(c, EV_FOCUS);
    }
}

void ctl_focus(control *c) {
    if (!c || !focusable(c))
        return;
    set_focus(c->win, c);
}

void ctl_set_handler(control *c, ctl_handler h, void *user) {
    if (!c)
        return;
    c->handler = h;
    c->user = user;
}

void ctl_set_colors(control *c, uint8_t fg, uint8_t bg) {
    if (!c)
        return;
    c->fg = fg;
    c->bg = bg;
}

void ctl_style_add(control *c, uint32_t style) {
    if (c)
        c->style |= style;
}

void ctl_move(control *c, int x, int y, int cw, int ch) {
    if (!c)
        return;
    int wx, wy;
    win_client_rect(c->win, &wx, &wy, &c->base_cw, &c->base_ch);
    c->x = x;
    c->y = y;
    c->w = cw;
    c->h = ch;
    ctl_layout(c);
}

void ctl_on_page(control *c, control *tabs, int page) {
    if (!c)
        return;
    c->tab_owner = tabs;
    c->tab_page = page;
}

control *ctl_by_id(window *w, int id) {
    for (control *c = w ? w->controls : NULL; c; c = c->next)
        if (c->id == id)
            return c;
    return NULL;
}

control *ctl_focused(window *w) { return w ? w->focus : NULL; }

/* ── lists ─────────────────────────────────────────────────────────────── */

static char *dupstr(const char *s) {
    int n = (int)strlen(s);
    char *d = (char *)kmalloc(n + 1);
    if (d)
        memcpy(d, s, n + 1);
    return d;
}

int list_add_data(control *c, const char *text, uint32_t data) {
    if (!c)
        return -1;
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 16;
        char **ni = (char **)kmalloc(cap * sizeof(char *));
        uint32_t *nd = (uint32_t *)kmalloc(cap * sizeof(uint32_t));
        if (!ni || !nd) {
            if (ni)
                kfree(ni);
            if (nd)
                kfree(nd);
            return -1;
        }
        if (c->count) {
            memcpy(ni, c->items, c->count * sizeof(char *));
            memcpy(nd, c->item_data, c->count * sizeof(uint32_t));
        }
        if (c->items)
            kfree(c->items);
        if (c->item_data)
            kfree(c->item_data);
        c->items = ni;
        c->item_data = nd;
        c->cap = cap;
    }
    char *d = dupstr(text ? text : "");
    if (!d)
        return -1;
    c->items[c->count] = d;
    c->item_data[c->count] = data;
    return c->count++;
}

int list_add(control *c, const char *text) { return list_add_data(c, text, 0); }

void list_set(control *c, int i, const char *text) {
    if (!c || i < 0 || i >= c->count)
        return;
    char *d = dupstr(text ? text : "");
    if (!d)
        return;
    kfree(c->items[i]);
    c->items[i] = d;
}

void list_remove(control *c, int i) {
    if (!c || i < 0 || i >= c->count)
        return;
    kfree(c->items[i]);
    for (int k = i; k < c->count - 1; k++) {
        c->items[k] = c->items[k + 1];
        c->item_data[k] = c->item_data[k + 1];
    }
    c->count--;
    if (c->sel >= c->count)
        c->sel = c->count - 1;
}

void list_clear(control *c) {
    if (!c)
        return;
    free_items(c);
    c->sel = -1;
    c->top = 0;
}

int list_count(const control *c) { return c ? c->count : 0; }

const char *list_get(const control *c, int i) {
    return (c && i >= 0 && i < c->count) ? c->items[i] : "";
}

uint32_t list_data(const control *c, int i) {
    return (c && i >= 0 && i < c->count) ? c->item_data[i] : 0;
}

int list_sel(const control *c) { return c ? c->sel : -1; }

static int list_rows(const control *c) {
    int h = c->ah - 4 - ((c->style & LBS_HEADER) ? TH_LINE_H + 1 : 0);
    int r = h / TH_LINE_H;
    return r < 1 ? 1 : r;
}

static void list_ensure_visible(control *c) {
    if (c->type != CTL_LIST)
        return;
    int rows = list_rows(c);
    int first = (c->style & LBS_HEADER) ? 1 : 0;
    if (c->sel >= 0) {
        if (c->sel - first < c->top)
            c->top = c->sel - first;
        if (c->sel - first >= c->top + rows)
            c->top = c->sel - first - rows + 1;
    }
    int maxtop = (c->count - first) - rows;
    if (c->top > maxtop)
        c->top = maxtop;
    if (c->top < 0)
        c->top = 0;
}

void list_set_sel(control *c, int i) {
    if (!c)
        return;
    int first = (c->type == CTL_LIST && (c->style & LBS_HEADER)) ? 1 : 0;
    if (i < first)
        i = (c->count > first && i >= 0) ? first : -1;
    if (i >= c->count)
        i = c->count - 1;
    c->sel = i;
    if (c->type == CTL_COMBO)
        set_text_raw(c, i >= 0 ? c->items[i] : "");
    list_ensure_visible(c);
}

void list_set_columns(control *c, const int *widths, int n) {
    if (!c)
        return;
    if (n > 8)
        n = 8;
    for (int i = 0; i < n; i++)
        c->cols[i] = widths[i];
    c->ncols = n;
}

/* draw one list row (columns split on '\t') */
static void list_row(const control *c, int x, int y, int w, const char *t,
                     uint8_t col) {
    if (c->ncols <= 0) {
        draw_string(x, y + 1, t, col, 2);
        return;
    }
    int cx = x;
    const char *p = t;
    for (int k = 0; k < c->ncols && *p != 0; k++) {
        const char *e = p;
        while (*e && *e != '\t')
            e++;
        int cw = c->cols[k] > 0 ? c->cols[k] : w - (cx - x);
        clip_push(cx, y, cw - 2, TH_LINE_H);
        draw_text_n(cx, y + 1, p, (int)(e - p), col, 2);
        clip_pop();
        cx += cw;
        p = *e ? e + 1 : e;
    }
}

/* ── drawing ───────────────────────────────────────────────────────────── */

static void draw_label(control *c) {
    int big = (c->style & LS_BIG) ? 1 : 2;
    int lh = big == 1 ? 10 : 8;
    if (c->bg != 0xFF)
        fill_rect(c->ax, c->ay, c->aw, c->ah, c->bg);
    if (c->style & CS_BORDER)
        th_sunken(c->ax, c->ay, c->aw, c->ah, c->bg != 0xFF ? c->bg : TH_FIELD);
    int pad = (c->style & CS_BORDER) ? 3 : 0;
    /* count lines for vertical centring of single-line labels */
    int lines = 1;
    for (const char *p = c->text; *p; p++)
        if (*p == '\n')
            lines++;
    int y = c->ay + pad;
    if (lines == 1)
        y = c->ay + (c->ah - (big == 1 ? 8 : 6)) / 2;
    const char *p = c->text;
    uint8_t col = fgc(c, TH_TEXT);
    while (1) {
        const char *e = p;
        while (*e && *e != '\n')
            e++;
        int n = (int)(e - p);
        int cw = big == 1 ? 8 : 5;
        int tw = n * cw;
        int x = c->ax + pad;
        if (c->style & LS_CENTER)
            x = c->ax + (c->aw - tw) / 2;
        else if (c->style & LS_RIGHT)
            x = c->ax + c->aw - pad - tw;
        draw_text_n(x, y, p, n, col, big);
        y += lh;
        if (!*e)
            break;
        p = e + 1;
    }
}

static void draw_button(control *c) {
    bool down = c->pressed || ((c->style & CS_TOGGLE) && c->value);
    uint8_t face = bgc(c, TH_FACE);
    if (c->style & CS_FLAT) {
        fill_rect(c->ax, c->ay, c->aw, c->ah, down ? TH_SHADOW : face);
        if (down || c->focused)
            draw_rect(c->ax, c->ay, c->aw, c->ah, BLACK);
    } else if (down) {
        /* toggled-on buttons get a light face so they stand out */
        bool on = (c->style & CS_TOGGLE) && c->value && !c->pressed;
        th_pressed(c->ax, c->ay, c->aw, c->ah, on ? TH_LIGHT : face);
    } else {
        th_raised(c->ax, c->ay, c->aw, c->ah, face);
    }
    if ((c->style & CS_DEFAULT) && !down)
        draw_rect(c->ax - 1, c->ay - 1, c->aw + 2, c->ah + 2, BLACK);
    int o = down ? 1 : 0;
    text_in(c->ax + o, c->ay + o, c->aw, c->ah, c->text, fgc(c, TH_TEXT), 1);
    if (c->focused)
        th_focus(c->ax + 2, c->ay + 2, c->aw - 4, c->ah - 4);
}

static void draw_check(control *c, bool radio) {
    int bx = c->ax, by = c->ay + (c->ah - 9) / 2;
    if (radio) {
        static const uint8_t ring[9][2] = {{3, 5}, {1, 7}, {1, 7},
                                           {0, 8}, {0, 8}, {0, 8},
                                           {1, 7}, {1, 7}, {3, 5}};
        for (int r = 0; r < 9; r++)
            draw_line(bx + ring[r][0], by + r, bx + ring[r][1], by + r,
                      c->enabled ? TH_FIELD : TH_FACE);
        for (int r = 0; r < 9; r++) {
            draw_dot(bx + ring[r][0], by + r, r < 5 ? TH_SHADOW : TH_LIGHT);
            draw_dot(bx + ring[r][1], by + r, r < 4 ? TH_SHADOW : TH_LIGHT);
        }
        draw_line(bx + 3, by, bx + 5, by, TH_SHADOW);
        draw_line(bx + 3, by + 8, bx + 5, by + 8, TH_LIGHT);
        if (c->value)
            fill_rect(bx + 3, by + 3, 3, 3, BLACK);
    } else {
        th_sunken(bx, by, 9, 9, c->enabled ? TH_FIELD : TH_FACE);
        if (c->value) {
            draw_line(bx + 2, by + 4, bx + 3, by + 5, BLACK);
            draw_line(bx + 2, by + 5, bx + 3, by + 6, BLACK);
            draw_line(bx + 4, by + 4, bx + 6, by + 2, BLACK);
            draw_line(bx + 4, by + 5, bx + 6, by + 3, BLACK);
        }
    }
    draw_string(bx + 13, c->ay + (c->ah - 6) / 2, c->text, fgc(c, TH_TEXT), 2);
    if (c->focused)
        th_focus(bx + 11, c->ay, (int)strlen(c->text) * 5 + 4, c->ah);
}

static void draw_list(control *c) {
    uint8_t bg = bgc(c, TH_FIELD);
    th_sunken(c->ax, c->ay, c->aw, c->ah, bg);
    int first = (c->style & LBS_HEADER) ? 1 : 0;
    int rows = list_rows(c);
    int n = c->count - first;
    bool sb = n > rows;
    int iw = c->aw - 4 - (sb ? SB_W : 0);
    int y = c->ay + 2;
    if (first && c->count > 0) {
        fill_rect(c->ax + 2, y, iw, TH_LINE_H, TH_FACE);
        list_row(c, c->ax + 4, y, iw - 2, c->items[0], BLACK);
        draw_line(c->ax + 2, y + TH_LINE_H, c->ax + 1 + iw, y + TH_LINE_H,
                  TH_SHADOW);
        y += TH_LINE_H + 1;
    }
    clip_push(c->ax + 2, y, iw, rows * TH_LINE_H);
    for (int r = 0; r < rows; r++) {
        int i = first + c->top + r;
        if (i >= c->count)
            break;
        uint8_t col = fgc(c, TH_TEXT);
        if (i == c->sel) {
            fill_rect(c->ax + 2, y, iw, TH_LINE_H,
                      c->focused ? TH_SEL_BG : TH_SHADOW);
            col = TH_SEL_FG;
        }
        list_row(c, c->ax + 4, y, iw - 2, c->items[i], col);
        y += TH_LINE_H;
    }
    clip_pop();
    if (sb)
        sb_draw(c->ax + c->aw - 2 - SB_W, c->ay + 2, c->ah - 4, true, c->top,
                n - rows, rows);
    if (c->focused && c->sel < 0)
        th_focus(c->ax + 2, c->ay + 2, iw, c->ah - 4);
}

static void combo_popup_rect(control *c, int *x, int *y, int *w, int *h) {
    int rows = c->count < POPUP_ROWS ? c->count : POPUP_ROWS;
    if (rows < 1)
        rows = 1;
    *x = c->ax;
    *w = c->aw;
    *h = rows * TH_LINE_H + 4;
    *y = c->ay + c->ah;
    if (*y + *h > SCREEN_HEIGHT - TASKBAR_H)
        *y = c->ay - *h;
}

static void draw_combo(control *c) {
    th_sunken(c->ax, c->ay, c->aw, c->ah,
              c->enabled ? bgc(c, TH_FIELD) : TH_FACE);
    int bw = c->ah - 2;
    int bx = c->ax + c->aw - bw - 1;
    if (c->open)
        th_pressed(bx, c->ay + 1, bw, c->ah - 2, TH_FACE);
    else
        th_raised(bx, c->ay + 1, bw, c->ah - 2, TH_FACE);
    arrow(bx + bw / 2 - 1, c->ay + c->ah / 2 - 1, 1, BLACK);
    clip_push(c->ax + 2, c->ay, c->aw - bw - 4, c->ah);
    if (c->focused && !c->open) {
        fill_rect(c->ax + 2, c->ay + 2, c->aw - bw - 4, c->ah - 4, TH_SEL_BG);
        text_in(c->ax + 3, c->ay, c->aw, c->ah, c->text, TH_SEL_FG, 0);
    } else {
        text_in(c->ax + 3, c->ay, c->aw, c->ah, c->text, fgc(c, TH_TEXT), 0);
    }
    clip_pop();
}

static void draw_combo_popup(control *c) {
    int x, y, w, h;
    combo_popup_rect(c, &x, &y, &w, &h);
    gfx_reset_clip();
    fill_rect(x, y, w, h, TH_FIELD);
    draw_rect(x, y, w, h, BLACK);
    int rows = (h - 4) / TH_LINE_H;
    if (c->top > c->count - rows)
        c->top = c->count - rows;
    if (c->top < 0)
        c->top = 0;
    clip_push(x + 1, y + 1, w - 2, h - 2);
    for (int r = 0; r < rows && c->top + r < c->count; r++) {
        int i = c->top + r;
        int ry = y + 2 + r * TH_LINE_H;
        uint8_t col = TH_TEXT;
        if (i == c->value) { /* value = highlighted row while open */
            fill_rect(x + 2, ry, w - 4, TH_LINE_H, TH_SEL_BG);
            col = TH_SEL_FG;
        }
        draw_string(x + 4, ry + 1, c->items[i], col, 2);
    }
    clip_pop();
}

static void draw_scroll(control *c) {
    bool v = c->type == CTL_VSCROLL;
    sb_draw(c->ax, c->ay, v ? c->ah : c->aw, v, c->value, c->max, c->page);
}

static void draw_slider(control *c) {
    int gy = c->ay + c->ah / 2 - 1;
    th_sunken(c->ax + 2, gy, c->aw - 4, 3, TH_SHADOW);
    int range = c->max - c->min;
    int span = c->aw - 7;
    int tx = c->ax + (range > 0 ? (c->value - c->min) * span / range : 0);
    th_raised(tx, c->ay, 7, c->ah, c->enabled ? TH_FACE : TH_SHADOW);
    if (c->focused)
        th_focus(c->ax, c->ay, c->aw, c->ah);
}

static void draw_progress(control *c) {
    th_sunken(c->ax, c->ay, c->aw, c->ah, TH_FIELD);
    int fw = c->max > 0 ? (c->aw - 4) * c->value / c->max : 0;
    if (fw > 0)
        fill_rect(c->ax + 2, c->ay + 2, fw, c->ah - 4,
                  c->fg != 0xFF ? c->fg : TH_SEL_BG);
}

static void draw_group(control *c) {
    int ty = c->ay + 3;
    draw_rect(c->ax, ty, c->aw, c->ah - 3, TH_SHADOW);
    draw_rect(c->ax + 1, ty + 1, c->aw - 2, c->ah - 5, TH_LIGHT);
    if (c->text[0]) {
        int tw = (int)strlen(c->text) * 5 + 4;
        fill_rect(c->ax + 6, c->ay, tw, 7, c->win->content_color);
        draw_string(c->ax + 8, c->ay, c->text, fgc(c, TH_TEXT), 2);
    }
}

static int tab_extent(const control *c, int i, int *x0) {
    /* tabs are as wide as their caption */
    int x = c->ax, k = 0;
    const char *p = c->text;
    while (*p || k <= i) {
        const char *e = p;
        while (*e && *e != '|')
            e++;
        int w = (int)(e - p) * 5 + 10;
        if (k == i) {
            *x0 = x;
            return w;
        }
        x += w;
        k++;
        if (!*e)
            break;
        p = e + 1;
    }
    *x0 = x;
    return 0;
}

static void draw_tabs(control *c) {
    int base = c->ay + c->ah - 1;
    draw_line(c->ax, base, c->ax + c->aw - 1, base, TH_LIGHT);
    const char *p = c->text;
    for (int i = 0; i < c->count; i++) {
        const char *e = p;
        while (*e && *e != '|')
            e++;
        int x0, w = tab_extent(c, i, &x0);
        bool sel = (i == c->value);
        int y0 = sel ? c->ay : c->ay + 2;
        fill_rect(x0, y0, w, base - y0 + (sel ? 1 : 0), TH_FACE);
        draw_line(x0, y0, x0 + w - 1, y0, TH_LIGHT);
        draw_line(x0, y0, x0, base, TH_LIGHT);
        draw_line(x0 + w - 1, y0, x0 + w - 1, base, BLACK);
        draw_text_n(x0 + 5, y0 + 3, p, (int)(e - p), fgc(c, TH_TEXT), 2);
        if (sel && c->focused)
            th_focus(x0 + 2, y0 + 2, w - 4, base - y0 - 2);
        p = *e ? e + 1 : e;
    }
}

static void draw_custom(control *c) {
    if (c->style & CS_BORDER) {
        th_sunken(c->ax, c->ay, c->aw, c->ah, bgc(c, TH_FIELD));
        clip_push(c->ax + 2, c->ay + 2, c->aw - 4, c->ah - 4);
        if (c->paint)
            c->paint(c, c->ax + 2, c->ay + 2, c->aw - 4, c->ah - 4);
        clip_pop();
    } else {
        if (c->bg != 0xFF)
            fill_rect(c->ax, c->ay, c->aw, c->ah, c->bg);
        if (c->paint)
            c->paint(c, c->ax, c->ay, c->aw, c->ah);
    }
    if (c->focused && (c->style & CS_FOCUSABLE) && (c->style & CS_BORDER))
        draw_rect(c->ax + 1, c->ay + 1, c->aw - 2, c->ah - 2, TH_SEL_BG);
}

void ctl_draw_all(window *w) {
    int cx, cy, cw, ch;
    win_client_rect(w, &cx, &cy, &cw, &ch);
    control *popup = NULL;
    for (control *c = w->controls; c; c = c->next) {
        ctl_layout(c);
        if (!ctl_shown(c))
            continue;
        gfx_set_clip(cx, cy, cw, ch);
        clip_push(c->ax - 1, c->ay - 1, c->aw + 2, c->ah + 2);
        switch (c->type) {
        case CTL_LABEL:
            draw_label(c);
            break;
        case CTL_BUTTON:
            draw_button(c);
            break;
        case CTL_CHECK:
            draw_check(c, false);
            break;
        case CTL_RADIO:
            draw_check(c, true);
            break;
        case CTL_EDIT:
            edit_draw(c);
            break;
        case CTL_LIST:
            draw_list(c);
            break;
        case CTL_COMBO:
            draw_combo(c);
            break;
        case CTL_VSCROLL:
        case CTL_HSCROLL:
            draw_scroll(c);
            break;
        case CTL_SLIDER:
            draw_slider(c);
            break;
        case CTL_PROGRESS:
            draw_progress(c);
            break;
        case CTL_GROUP:
            draw_group(c);
            break;
        case CTL_TABS:
            draw_tabs(c);
            break;
        case CTL_CUSTOM:
            draw_custom(c);
            break;
        }
        clip_pop();
        if (c == s_popup)
            popup = c;
    }
    gfx_reset_clip();
    if (popup)
        draw_combo_popup(popup);
    gfx_reset_clip();
}

/* ── input ─────────────────────────────────────────────────────────────── */

bool ctl_mouse_captured(void) { return s_capture != NULL; }

static control *hit(window *w, int mx, int my) {
    control *found = NULL;
    for (control *c = w->controls; c; c = c->next) {
        ctl_layout(c);
        if (!ctl_shown(c) || !c->enabled)
            continue;
        if (c->type == CTL_LABEL || c->type == CTL_GROUP ||
            c->type == CTL_PROGRESS)
            continue;
        if (inside(mx, my, c->ax, c->ay, c->aw, c->ah))
            found = c; /* last created wins (drawn on top) */
    }
    return found;
}

static window *window_at(int mx, int my) {
    for (int i = win_count - 1; i >= 0; i--) {
        window *w = win_stack[i];
        if (!w->visible || w->minimized)
            continue;
        int wy = w->y + MENUBAR_H;
        if (mx >= w->x && mx < w->x + w->w && my >= wy && my < wy + w->h)
            return w;
    }
    return NULL;
}

static void combo_close(control *c, bool pick) {
    c->open = false;
    if (s_popup == c)
        s_popup = NULL;
    if (pick && c->value >= 0 && c->value < c->count && c->value != c->sel) {
        list_set_sel(c, c->value);
        ctl_invoke(c, EV_CHANGE);
    }
}

static void combo_open(control *c) {
    if (s_popup && s_popup != c)
        combo_close(s_popup, false);
    c->open = true;
    c->value = c->sel >= 0 ? c->sel : 0;
    c->top = c->value - POPUP_ROWS / 2;
    if (c->top < 0)
        c->top = 0;
    s_popup = c;
}

static void list_scroll_to(control *c, int top) {
    c->top = top;
    list_ensure_visible(c);
}

/* Handlers may destroy c (or its window). The tail below therefore tests
 * the saved type, and only slider/tabs/custom (which have not called a
 * handler yet at that point) touch c again. */
static void mouse_down(control *c) {
    ctl_type t = c->type;
    int mx = mouse.x - c->ax, my = mouse.y - c->ay;
    c->mx = mx;
    c->my = my;
    uint32_t now = time_millis();
    bool dbl = (c == s_last_down && now - s_last_down_t < DBLCLICK_MS);
    s_last_down = c;
    s_last_down_t = now;
    s_drag_part = 0;

    switch (c->type) {
    case CTL_BUTTON:
    case CTL_CHECK:
    case CTL_RADIO:
        c->pressed = true;
        break;
    case CTL_EDIT:
        edit_mouse(c, mx, my, true, dbl);
        break;
    case CTL_LIST: {
        int first = (c->style & LBS_HEADER) ? 1 : 0;
        int rows = list_rows(c);
        int n = c->count - first;
        if (n > rows && mx >= c->aw - 2 - SB_W) {
            int top = sb_mouse(my - 2, c->ah - 4, c->top, n - rows, rows, true,
                               &s_drag_part);
            list_scroll_to(c, top);
            s_drag_part = s_drag_part ? s_drag_part : 1;
            s_drag_part += 10; /* mark: scrollbar drag */
            break;
        }
        int ry = my - 2 - (first ? TH_LINE_H + 1 : 0);
        if (ry < 0)
            break;
        int i = first + c->top + ry / TH_LINE_H;
        if (i >= c->count)
            break;
        bool same = (i == c->sel);
        if (!same) {
            c->sel = i;
            list_ensure_visible(c);
            ctl_invoke(c, EV_CHANGE);
        }
        if (dbl && same && s_last_down_item == i) {
            s_last_down = NULL;
            ctl_invoke(c, EV_SUBMIT);
            return;
        }
        s_last_down_item = i;
        break;
    }
    case CTL_COMBO:
        if (c->open)
            combo_close(c, false);
        else
            combo_open(c);
        break;
    case CTL_VSCROLL:
    case CTL_HSCROLL: {
        bool v = c->type == CTL_VSCROLL;
        int nv = sb_mouse(v ? my : mx, v ? c->ah : c->aw, c->value, c->max,
                          c->page, true, &s_drag_part);
        if (nv != c->value) {
            c->value = nv;
            ctl_invoke(c, EV_CHANGE);
        }
        break;
    }
    case CTL_SLIDER:
    case CTL_TABS:
    case CTL_CUSTOM:
        break;
    default:
        break;
    }
    if (t == CTL_SLIDER) {
        int range = c->max - c->min, span = c->aw - 7;
        int v = c->min + (span > 0 ? (mx - 3) * range / span : 0);
        if (v < c->min)
            v = c->min;
        if (v > c->max)
            v = c->max;
        if (v != c->value) {
            c->value = v;
            ctl_invoke(c, EV_CHANGE);
        }
    } else if (t == CTL_TABS) {
        for (int i = 0; i < c->count; i++) {
            int x0, w = tab_extent(c, i, &x0);
            if (mouse.x >= x0 && mouse.x < x0 + w) {
                if (i != c->value) {
                    c->value = i;
                    ctl_invoke(c, EV_CHANGE);
                }
                break;
            }
        }
    } else if (t == CTL_CUSTOM) {
        ctl_invoke(c, dbl ? EV_DBLCLICK : EV_MOUSEDOWN);
    }
}

static void mouse_move(control *c) {
    int mx = mouse.x - c->ax, my = mouse.y - c->ay;
    c->mx = mx;
    c->my = my;
    switch (c->type) {
    case CTL_BUTTON:
    case CTL_CHECK:
    case CTL_RADIO:
        c->pressed = inside(mouse.x, mouse.y, c->ax, c->ay, c->aw, c->ah);
        break;
    case CTL_EDIT:
        if (mouse.dx || mouse.dy)
            edit_mouse(c, mx, my, false, false);
        break;
    case CTL_LIST:
        if (s_drag_part == 15) {
            int first = (c->style & LBS_HEADER) ? 1 : 0;
            int rows = list_rows(c), n = c->count - first;
            int part = 5;
            list_scroll_to(c, sb_mouse(my - 2, c->ah - 4, c->top, n - rows,
                                       rows, false, &part));
        }
        break;
    case CTL_VSCROLL:
    case CTL_HSCROLL:
        if (s_drag_part == 5) {
            bool v = c->type == CTL_VSCROLL;
            int nv = sb_mouse(v ? my : mx, v ? c->ah : c->aw, c->value, c->max,
                              c->page, false, &s_drag_part);
            if (nv != c->value) {
                c->value = nv;
                ctl_invoke(c, EV_CHANGE);
            }
        }
        break;
    case CTL_SLIDER: {
        int range = c->max - c->min, span = c->aw - 7;
        int v = c->min + (span > 0 ? (mx - 3) * range / span : 0);
        if (v < c->min)
            v = c->min;
        if (v > c->max)
            v = c->max;
        if (v != c->value) {
            c->value = v;
            ctl_invoke(c, EV_CHANGE);
        }
        break;
    }
    case CTL_CUSTOM:
        if (mouse.dx || mouse.dy)
            ctl_invoke(c, EV_MOUSEMOVE);
        break;
    default:
        break;
    }
}

/* NOTE: the handler may destroy c (or its window): never touch c after. */
static void mouse_up(control *c) {
    bool in = inside(mouse.x, mouse.y, c->ax, c->ay, c->aw, c->ah);
    c->mx = mouse.x - c->ax;
    c->my = mouse.y - c->ay;
    switch (c->type) {
    case CTL_BUTTON:
        c->pressed = false;
        if (in) {
            if (c->style & CS_TOGGLE)
                c->value = !c->value;
            ctl_invoke(c, EV_CLICK);
        }
        break;
    case CTL_CHECK:
        c->pressed = false;
        if (in) {
            c->value = !c->value;
            ctl_invoke(c, EV_CHANGE);
        }
        break;
    case CTL_RADIO:
        c->pressed = false;
        if (in && !c->value) {
            radio_pick(c);
            ctl_invoke(c, EV_CHANGE);
        }
        break;
    case CTL_CUSTOM:
        ctl_invoke(c, EV_MOUSEUP);
        break;
    default:
        break;
    }
}

static void click(control *c) {
    switch (c->type) {
    case CTL_BUTTON:
        if (c->style & CS_TOGGLE)
            c->value = !c->value;
        ctl_invoke(c, EV_CLICK);
        break;
    case CTL_CHECK:
        c->value = !c->value;
        ctl_invoke(c, EV_CHANGE);
        break;
    case CTL_RADIO:
        if (!c->value) {
            radio_pick(c);
            ctl_invoke(c, EV_CHANGE);
        }
        break;
    default:
        break;
    }
}

static bool step_value(control *c, int delta) {
    int v = c->value + delta;
    int lo = c->type == CTL_SLIDER ? c->min : 0;
    int hi = c->type == CTL_TABS ? c->count - 1 : c->max;
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    if (v != c->value) {
        c->value = v;
        ctl_invoke(c, EV_CHANGE);
    }
    return true;
}

static bool list_key(control *c, uint8_t sc, char ch) {
    int first = (c->style & LBS_HEADER) ? 1 : 0;
    int rows = list_rows(c);
    int sel = c->sel < first ? first - 1 : c->sel;
    int ns = sel;
    switch (sc) {
    case UP_ARROW:
        ns = sel - 1;
        break;
    case DOWN_ARROW:
        ns = sel + 1;
        break;
    case PAGE_UP:
        ns = sel - rows;
        break;
    case PAGE_DOWN:
        ns = sel + rows;
        break;
    case HOME:
        ns = first;
        break;
    case END:
        ns = c->count - 1;
        break;
    case ENTER:
        if (c->sel >= first) {
            ctl_invoke(c, EV_SUBMIT);
            return true;
        }
        return false;
    default:
        if (ch > ' ' && ch < 127) { /* jump to next item starting with ch */
            char lc = ch | 0x20;
            for (int k = 1; k <= c->count; k++) {
                int i = (sel + k) % c->count;
                if (i < first || !c->items[i][0])
                    continue;
                if ((c->items[i][0] | 0x20) == lc) {
                    ns = i;
                    break;
                }
            }
            if (ns == sel)
                return true;
            break;
        }
        return false; /* Esc, F-keys...: window / app */
    }
    if (c->count <= first)
        return true;
    if (ns < first)
        ns = first;
    if (ns >= c->count)
        ns = c->count - 1;
    if (ns != c->sel) {
        c->sel = ns;
        list_ensure_visible(c);
        ctl_invoke(c, EV_CHANGE);
    }
    return true;
}

static bool combo_key(control *c, uint8_t sc) {
    if (c->open) {
        switch (sc) {
        case UP_ARROW:
            if (c->value > 0)
                c->value--;
            break;
        case DOWN_ARROW:
            if (c->value < c->count - 1)
                c->value++;
            break;
        case ENTER:
        case SPACE:
            combo_close(c, true);
            return true;
        case ESC:
            combo_close(c, false);
            return true;
        default:
            return true;
        }
        if (c->value < c->top)
            c->top = c->value;
        if (c->value >= c->top + POPUP_ROWS)
            c->top = c->value - POPUP_ROWS + 1;
        return true;
    }
    int ns = c->sel;
    if (sc == UP_ARROW)
        ns--;
    else if (sc == DOWN_ARROW)
        ns++;
    else if (sc == SPACE || (sc == DOWN_ARROW && kb.alt)) {
        combo_open(c);
        return true;
    } else
        return false;
    if (ns < 0 || ns >= c->count)
        return true;
    list_set_sel(c, ns);
    ctl_invoke(c, EV_CHANGE);
    return true;
}

/* returns true if the key was used */
static bool key_to(control *c) {
    uint8_t sc = kb.last_scancode;
    char ch = kb.last_char;
    switch (c->type) {
    case CTL_BUTTON:
    case CTL_CHECK:
    case CTL_RADIO:
        if (sc == SPACE || (sc == ENTER && c->type == CTL_BUTTON)) {
            click(c);
            return true;
        }
        if (c->type == CTL_RADIO && (sc == UP_ARROW || sc == DOWN_ARROW ||
                                     sc == LEFT_ARROW || sc == RIGHT_ARROW)) {
            /* move within the group */
            control *prev = NULL, *next = NULL, *firstc = NULL, *last = NULL;
            bool seen = false;
            for (control *o = c->win->controls; o; o = o->next) {
                if (o->type != CTL_RADIO || o->group != c->group ||
                    !focusable(o))
                    continue;
                if (!firstc)
                    firstc = o;
                last = o;
                if (o == c)
                    seen = true;
                else if (!seen)
                    prev = o;
                else if (!next)
                    next = o;
            }
            bool fwd = sc == DOWN_ARROW || sc == RIGHT_ARROW;
            control *t = fwd ? (next ? next : firstc) : (prev ? prev : last);
            if (t && t != c) {
                set_focus(c->win, t);
                click(t);
            }
            return true;
        }
        return false;
    case CTL_EDIT:
        return edit_key(c);
    case CTL_LIST:
        return list_key(c, sc, ch);
    case CTL_COMBO:
        return combo_key(c, sc);
    case CTL_VSCROLL:
    case CTL_HSCROLL:
    case CTL_SLIDER: {
        int big = c->type == CTL_SLIDER ? (c->max - c->min) / 10 : c->page;
        if (big < 1)
            big = 1;
        switch (sc) {
        case UP_ARROW:
        case LEFT_ARROW:
            return step_value(c, -1);
        case DOWN_ARROW:
        case RIGHT_ARROW:
            return step_value(c, 1);
        case PAGE_UP:
            return step_value(c, -big);
        case PAGE_DOWN:
            return step_value(c, big);
        case HOME:
            return step_value(c, -1000000);
        case END:
            return step_value(c, 1000000);
        default:
            return false;
        }
    }
    case CTL_TABS:
        if (sc == LEFT_ARROW)
            return step_value(c, -1);
        if (sc == RIGHT_ARROW)
            return step_value(c, 1);
        return false;
    case CTL_CUSTOM:
        ctl_invoke(c, EV_KEY);
        return true;
    default:
        return false;
    }
}

static void focus_step(window *w, bool back) {
    control *list[64];
    int n = 0, cur = -1;
    for (control *c = w->controls; c && n < 64; c = c->next)
        if (focusable(c)) {
            /* radio groups: only the selected (or first) one is a stop */
            if (c->type == CTL_RADIO && !c->value && c != w->focus) {
                bool other_selected = false;
                for (control *o = w->controls; o; o = o->next)
                    if (o->type == CTL_RADIO && o->group == c->group &&
                        o->value && focusable(o))
                        other_selected = true;
                if (other_selected)
                    continue;
            }
            if (c == w->focus)
                cur = n;
            list[n++] = c;
        }
    if (!n)
        return;
    int next = cur < 0 ? (back ? n - 1 : 0) : (cur + (back ? n - 1 : 1)) % n;
    control *t = list[next];
    set_focus(w, t);
    if (t->type == CTL_EDIT && !(t->style & ES_MULTILINE))
        edit_select_all(t);
}

static control *find_style(window *w, uint32_t st) {
    for (control *c = w->controls; c; c = c->next)
        if ((c->style & st) && c->type == CTL_BUTTON && ctl_shown(c) &&
            c->enabled)
            return c;
    return NULL;
}

static void hover_cursor(void) {
    static bool ours;
    window *w = window_at(mouse.x, mouse.y);
    control *c = (w && !w->modal_child && w->controls && !s_capture)
                     ? hit(w, mouse.x, mouse.y)
                     : (s_capture ? s_capture : NULL);
    if (c && c->type == CTL_EDIT) {
        CHOSEN_CURSOR = 3; /* I-beam over text fields */
        ours = true;
    } else if (ours) {
        CHOSEN_CURSOR = 0;
        ours = false;
    }
}

void ctl_dispatch(void) {
    if (modal_active())
        return;
    hover_cursor();

    /* 1) mouse held on a control */
    if (s_capture) {
        control *c = s_capture;
        if (mouse.left) {
            mouse_move(c);
        } else {
            s_capture = NULL;
            mouse_up(c);
        }
        mouse.left_clicked = false;
        return;
    }

    /* 2) open combo popup gets the first click */
    if (s_popup && mouse.left_clicked) {
        control *c = s_popup;
        int x, y, w, h;
        combo_popup_rect(c, &x, &y, &w, &h);
        if (inside(mouse.x, mouse.y, x, y, w, h)) {
            int r = (mouse.y - y - 2) / TH_LINE_H;
            int i = c->top + r;
            if (i >= 0 && i < c->count)
                c->value = i;
            combo_close(c, true);
            mouse.left_clicked = false;
            return;
        }
        if (!inside(mouse.x, mouse.y, c->ax, c->ay, c->aw, c->ah)) {
            combo_close(c, false);
            /* fall through: the click still does its normal job */
        }
    }
    if (s_popup && s_popup->win != wm_focused_window())
        combo_close(s_popup, false);

    /* 3) new click on a control */
    if (mouse.left_clicked && !g_menubar_click_consumed &&
        mouse.y >= MENUBAR_H) {
        window *w = window_at(mouse.x, mouse.y);
        if (w && w->modal_child) { /* window has a dialog open */
            wm_focus(w->modal_child);
            mouse.left_clicked = false;
            return;
        }
        if (w && w->controls) {
            int cx, cy, cw, ch;
            win_client_rect(w, &cx, &cy, &cw, &ch);
            bool on_grip = w->resizable && mouse.x >= w->x + w->w - 10 &&
                           mouse.y >= w->y + MENUBAR_H + w->h - 10;
            if (!on_grip && inside(mouse.x, mouse.y, cx, cy, cw, ch)) {
                control *c = hit(w, mouse.x, mouse.y);
                if (c) {
                    if (wm_focused_window() != w)
                        wm_focus(w);
                    if (focusable(c))
                        set_focus(w, c);
                    s_capture = c;
                    mouse.left_clicked = false;
                    mouse_down(c); /* may destroy c */
                    return;
                }
            }
        }
    }

    /* 4) keyboard → focused control of the focused window */
    if (!kb.key_pressed)
        return;
    window *w = wm_focused_window();
    if (!w || !w->visible || w->minimized || !w->controls)
        return;
    if (w->modal_child)
        return;
    if (s_popup && s_popup->win == w) {
        combo_key(s_popup, kb.last_scancode);
        kb.key_pressed = false;
        return;
    }
    control *f = w->focus;
    if (f && !focusable(f)) {
        set_focus(w, NULL);
        f = NULL;
    }
    if (kb.last_scancode == TAB && !kb.ctrl &&
        !(f && f->type == CTL_EDIT && (f->style & ES_MULTILINE) &&
          !(f->style & ES_READONLY) && !kb.shift)) {
        focus_step(w, kb.shift);
        kb.key_pressed = false;
        return;
    }
    if (f && key_to(f)) {
        kb.key_pressed = false;
        return;
    }
    if (kb.last_scancode == ENTER) {
        control *d = find_style(w, CS_DEFAULT);
        if (d) {
            kb.key_pressed = false;
            click(d);
            return;
        }
    }
    if (kb.last_scancode == ESC) {
        control *d = find_style(w, CS_CANCEL);
        if (d) {
            kb.key_pressed = false;
            click(d);
            return;
        }
    }
}

/* ── app windows ───────────────────────────────────────────────────────── */

void *ui_active_state(void) {
    window *w = wm_focused_window();
    if (w && w->modal_owner)
        w = w->modal_owner;
    return w ? w->userdata : NULL;
}

void win_set_userdata(window *w, void *ud) {
    if (!w)
        return;
    for (control *c = w->controls; c; c = c->next)
        if (c->user == w->userdata)
            c->user = ud;
    w->userdata = ud;
}

window *app_window(void *state, const char *title, int x, int y, int w, int h,
                   uint8_t bar_color, uint32_t flags) {
    window_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.x = x;
    spec.y = y;
    spec.w = w;
    spec.h = h;
    spec.title = title;
    spec.title_color = WHITE;
    spec.bar_color = bar_color;
    spec.content_color = TH_FACE;
    spec.visible = !(flags & AW_HIDDEN);
    spec.resizable = (flags & AW_RESIZABLE) != 0;
    spec.min_w = w * 2 / 3;
    spec.min_h = h * 2 / 3;
    spec.max_w = SCREEN_WIDTH;
    spec.max_h = SCREEN_HEIGHT;
    window *win = wm_register(&spec);
    if (!win)
        return NULL;
    if (flags & AW_NO_ANIM)
        win->anim_state = WIN_ANIM_NONE;
    win->userdata = state;
    return win;
}
