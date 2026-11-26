// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* the selection box (lib/selbox.h, where it is explained). Built into the
 * kernel and into the apps' library (libasmos.a): it draws with
 * draw_dot / fill_rect, which both have. */

#include "lib/selbox.h"
#include "lib/graphics.h"
#include "os/appapi.h"

static int iabs(int v) { return v < 0 ? -v : v; }

sb_part_t selbox_hit(const selbox_t *b, int x, int y) {
    if (!b->on)
        return SB_NONE;
    int g = b->grip > 0 ? b->grip : 2;
    /* outside: the whole grip; inside: less on a small box, so its
     * middle still moves it */
    int gx = (b->w - 1) / 3 < g ? (b->w - 1) / 3 : g;
    int gy = (b->h - 1) / 3 < g ? (b->h - 1) / 3 : g;
    int l = b->x, r = b->x + b->w - 1, t = b->y, bt = b->y + b->h - 1;
    if (x < l - g || x > r + g || y < t - g || y > bt + g)
        return SB_NONE;
    bool near_l = x <= l + gx, near_r = x >= r - gx;
    bool near_t = y <= t + gy, near_b = y >= bt - gy;
    if (near_l && near_r) /* (both, on a thin box: the nearer; ties grow) */
        near_l = iabs(x - l) < iabs(x - r);
    if (near_t && near_b)
        near_t = iabs(y - t) < iabs(y - bt);
    near_r = near_r && !near_l;
    near_b = near_b && !near_t;
    if (near_t)
        return near_l ? SB_NW : near_r ? SB_NE : SB_N;
    if (near_b)
        return near_l ? SB_SW : near_r ? SB_SE : SB_S;
    if (near_l)
        return SB_W;
    if (near_r)
        return SB_E;
    return SB_INSIDE;
}

sb_part_t selbox_down(selbox_t *b, int x, int y) {
    sb_part_t p = selbox_hit(b, x, y);
    b->dragging = true;
    b->part = p;
    b->ax = x;
    b->ay = y;
    if (p == SB_NONE) { /* a new one, from here */
        b->on = b->making = true;
        b->x = x;
        b->y = y;
        b->w = b->h = 1;
    }
    b->ox = b->x;
    b->oy = b->y;
    b->ow = b->w;
    b->oh = b->h;
    return p;
}

bool selbox_drag(selbox_t *b, int x, int y, bool keep) {
    if (!b->dragging || !b->on)
        return false;
    int dx = x - b->ax, dy = y - b->ay;
    int nx = b->x, ny = b->y, nw = b->w, nh = b->h;
    int mw = b->min_w > 0 ? b->min_w : 1, mh = b->min_h > 0 ? b->min_h : 1;
    sb_part_t p = b->part;
    if (b->making) {
        nw = iabs(dx) + 1;
        nh = iabs(dy) + 1;
        if (keep) /* a square */
            nw = nh = nw > nh ? nw : nh;
        nx = dx < 0 ? b->ax - nw + 1 : b->ax;
        ny = dy < 0 ? b->ay - nh + 1 : b->ay;
    } else if (p == SB_INSIDE) {
        nx = b->ox + dx;
        ny = b->oy + dy;
    } else {
        bool west = p == SB_W || p == SB_NW || p == SB_SW;
        bool east = p == SB_E || p == SB_NE || p == SB_SE;
        bool north = p == SB_N || p == SB_NW || p == SB_NE;
        bool south = p == SB_S || p == SB_SW || p == SB_SE;
        int l = b->ox, t = b->oy, r = b->ox + b->ow - 1, bt = b->oy + b->oh - 1;
        if (west)
            l = l + dx > r - mw + 1 ? r - mw + 1 : l + dx;
        if (east)
            r = r + dx < l + mw - 1 ? l + mw - 1 : r + dx;
        if (north)
            t = t + dy > bt - mh + 1 ? bt - mh + 1 : t + dy;
        if (south)
            bt = bt + dy < t + mh - 1 ? t + mh - 1 : bt + dy;
        nw = r - l + 1;
        nh = bt - t + 1;
        if (keep && (west || east) && (north || south)) {
            /* the shape it had: the bigger change wins */
            if (nw * b->oh > nh * b->ow)
                nh = nw * b->oh / b->ow;
            else
                nw = nh * b->ow / b->oh;
            nw = nw < mw ? mw : nw;
            nh = nh < mh ? mh : nh;
            if (west)
                l = r - nw + 1;
            if (north)
                t = bt - nh + 1;
        }
        nx = l;
        ny = t;
    }
    bool changed = nx != b->x || ny != b->y || nw != b->w || nh != b->h;
    b->x = nx;
    b->y = ny;
    b->w = nw;
    b->h = nh;
    return changed;
}

bool selbox_up(selbox_t *b) {
    if (b->dragging && b->making && b->w == 1 && b->h == 1)
        b->on = false; /* a click, not a box */
    b->dragging = b->making = false;
    return b->on;
}

int selbox_pointer(sb_part_t part) {
    switch (part) {
    case SB_INSIDE:
        return APP_POINTER_MOVE;
    case SB_N:
    case SB_S:
        return APP_POINTER_SIZE_NS;
    case SB_E:
    case SB_W:
        return APP_POINTER_SIZE_WE;
    case SB_NW:
    case SB_SE:
        return APP_POINTER_SIZE_NWSE;
    case SB_NE:
    case SB_SW:
        return APP_POINTER_SIZE_NESW;
    default:
        return -1;
    }
}

int selbox_pointer_at(const selbox_t *b, int x, int y) {
    if (b->dragging)
        return b->making ? -1 : selbox_pointer(b->part);
    return selbox_pointer(selbox_hit(b, x, y));
}

static void dot(int x, int y, uint8_t c, int cx, int cy, int cw, int ch) {
    if (x >= cx && y >= cy && x < cx + cw && y < cy + ch)
        draw_dot(x, y, c);
}

/* a handle: 3x3, black round a white middle */
static void handle(int x, int y, int cx, int cy, int cw, int ch) {
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
            dot(x + i, y + j, i || j ? BLACK : WHITE, cx, cy, cw, ch);
}

void selbox_draw(const selbox_t *b, int sx, int sy, int zoom, int cx, int cy,
                 int cw, int ch) {
    if (!b->on)
        return;
    zoom = zoom < 1 ? 1 : zoom;
    int l = sx + b->x * zoom, t = sy + b->y * zoom;
    int r = sx + (b->x + b->w) * zoom - 1, bt = sy + (b->y + b->h) * zoom - 1;
    for (int i = l; i <= r; i++) { /* black and white, 2 by 2 */
        dot(i, t, ((i + t) >> 1) & 1 ? BLACK : WHITE, cx, cy, cw, ch);
        dot(i, bt, ((i + bt) >> 1) & 1 ? BLACK : WHITE, cx, cy, cw, ch);
    }
    for (int j = t; j <= bt; j++) {
        dot(l, j, ((l + j) >> 1) & 1 ? BLACK : WHITE, cx, cy, cw, ch);
        dot(r, j, ((r + j) >> 1) & 1 ? BLACK : WHITE, cx, cy, cw, ch);
    }
    if (b->making)
        return;
    int mx = (l + r) / 2, my = (t + bt) / 2;
    handle(l, t, cx, cy, cw, ch);
    handle(r, t, cx, cy, cw, ch);
    handle(l, bt, cx, cy, cw, ch);
    handle(r, bt, cx, cy, cw, ch);
    if (r - l >= 8) {
        handle(mx, t, cx, cy, cw, ch);
        handle(mx, bt, cx, cy, cw, ch);
    }
    if (bt - t >= 8) {
        handle(l, my, cx, cy, cw, ch);
        handle(r, my, cx, cy, cw, ch);
    }
}
