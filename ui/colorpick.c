// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* The colour picker (dlg_color, ui/dialogs.h).
 *
 *   +--------------------+--+  +--------+
 *   | hue ->             |  |  | colour |   Red   [ 63]
 *   | saturation         |v |  +--------+   Green [ 21]
 *   |   (down: greyer)   |  |                Blue  [  0]
 *   +--------------------+--+
 *   [16 basic colours]                [OK] [Cancel]
 *
 * Any colour can be chosen (6 bits a channel, as the VGA has), though the
 * screen shows the field with its own fixed colours, dotted (ordered
 * dithering). The colour itself shows exactly in the preview when the
 * caller gives a palette index to try it in (a custom one, 248..255, on a
 * 256-colour screen); Cancel puts that back. */

#include "drivers/gpu.h"
#include "lib/graphics.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "ui/controls.h"
#include "ui/dialogs.h"
#include "ui/window.h"

#define FW 144 /* the field: hue across */
#define FH 64  /* ... saturation down */
#define BW 10  /* the value bar */
#define PW 52  /* the preview */
#define PH 30
#define HUES 1536 /* hue steps: 6 sides x 256 */

typedef struct {
    window *owner;
    dlg_color_cb cb;
    void *user;
    int preview; /* a palette index to try the colour in, or -1 */
    uint8_t old[3], rgb[3];
    int hue, sat, val; /* 0..HUES-1, 0..255, 0..63 */
    control *field, *bar, *box, *edit[3];
    bool quiet; /* setting the edits: not a change by the user */
    uint8_t pal[256][3];
    int n;              /* screen colours to dot with (16 or 248) */
    uint8_t near[4096]; /* 4 bits a channel -> nearest + 1 (0: not yet) */
    uint8_t fpix[FW * FH], bpix[BW * FH], ppix[PW * PH];
} pick_t;

static const uint8_t BASIC[16] = {0, 1, 2,  3,  4,  5,  6,  7,
                                  8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t BAYER[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// colour maths (6 bits a channel)

static void hsv_rgb(int h, int s, int v, uint8_t *o) {
    int side = h / 256, f = h % 256; /* side of the hexagon, how far */
    int p = v * (255 - s) / 255;
    int q = v * (255 * 255 - s * f) / (255 * 255);
    int t = v * (255 * 255 - s * (255 - f)) / (255 * 255);
    int r, g, b;
    switch (side) {
    case 0:
        r = v, g = t, b = p;
        break;
    case 1:
        r = q, g = v, b = p;
        break;
    case 2:
        r = p, g = v, b = t;
        break;
    case 3:
        r = p, g = q, b = v;
        break;
    case 4:
        r = t, g = p, b = v;
        break;
    default:
        r = v, g = p, b = q;
    }
    o[0] = (uint8_t)r, o[1] = (uint8_t)g, o[2] = (uint8_t)b;
}

static void rgb_hsv(const uint8_t *c, int *h, int *s, int *v) {
    int r = c[0], g = c[1], b = c[2];
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn;
    *v = mx;
    if (!mx || !d) { /* a grey: keep the hue it had */
        *s = 0;
        return;
    }
    *s = d * 255 / mx;
    int hh;
    if (mx == r)
        hh = (g - b) * 256 / d; /* -256..256 */
    else if (mx == g)
        hh = 512 + (b - r) * 256 / d;
    else
        hh = 1024 + (r - g) * 256 / d;
    *h = (hh + HUES) % HUES;
}

/* the screen colour to show (r, g, b) with at (x, y): dotted */
static uint8_t dot(pick_t *p, const uint8_t *c, int x, int y) {
    int k = p->n > 16 ? 1 : 2; /* how far apart the screen's colours are */
    int o = ((int)BAYER[y & 3][x & 3] * 2 - 15) * k / 2;
    int q[3];
    for (int i = 0; i < 3; i++) {
        int v = c[i] + o;
        q[i] = (v < 0 ? 0 : v > 63 ? 63 : v) >> 2;
    }
    int key = q[0] << 8 | q[1] << 4 | q[2];
    if (!p->near[key]) {
        int best = 0, bd = 0x7FFFFFFF;
        for (int i = 0; i < p->n; i++) {
            int d = 0;
            for (int j = 0; j < 3; j++) {
                int e = q[j] * 4 + 2 - p->pal[i][j];
                d += e * e * (j == 1 ? 4 : j == 0 ? 2 : 3);
            }
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
        p->near[key] = (uint8_t)(best + 1);
    }
    return (uint8_t)(p->near[key] - 1);
}

static bool live(const pick_t *p) { return p->preview >= GPU_CUSTOM; }

// the pictures

static void make_field(pick_t *p) {
    uint8_t c[3];
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) {
            hsv_rgb(x * HUES / FW, 255 - y * 255 / (FH - 1), 63, c);
            p->fpix[y * FW + x] = dot(p, c, x, y);
        }
}

static void make_bar(pick_t *p) {
    uint8_t c[3];
    for (int y = 0; y < FH; y++) {
        hsv_rgb(p->hue, p->sat, 63 - y * 63 / (FH - 1), c);
        for (int x = 0; x < BW; x++)
            p->bpix[y * BW + x] = dot(p, c, x, y);
    }
}

static void make_box(pick_t *p) {
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++)
            p->ppix[y * PW + x] = dot(p, p->rgb, x, y);
}

static void paint_field(control *c, int x, int y, int w, int h) {
    pick_t *p = c->user;
    (void)w, (void)h;
    draw_pixels(x, y, FW, FH, p->fpix, FW);
    int mx = x + p->hue * FW / HUES, my = y + (255 - p->sat) * (FH - 1) / 255;
    draw_rect(mx - 3, my - 3, 7, 7, BLACK); /* where it is: a small box */
    draw_rect(mx - 2, my - 2, 5, 5, WHITE);
}

static void paint_bar(control *c, int x, int y, int w, int h) {
    pick_t *p = c->user;
    (void)w, (void)h;
    draw_pixels(x, y, BW, FH, p->bpix, BW);
    int my = y + (63 - p->val) * (FH - 1) / 63;
    fill_rect(x - 3, my - 1, 3, 3, BLACK); /* a mark each side */
    fill_rect(x + BW, my - 1, 3, 3, BLACK);
}

static void paint_box(control *c, int x, int y, int w, int h) {
    pick_t *p = c->user;
    if (live(p)) /* the colour itself */
        fill_rect(x, y, w, h, (uint8_t)p->preview);
    else
        draw_pixels(x, y, PW < w ? PW : w, PH < h ? PH : h, p->ppix, PW);
}

// changes

/* the colour is p->rgb: everything else follows */
static void show(pick_t *p, bool edits) {
    if (live(p))
        gpu_set_color(p->preview, p->rgb[0], p->rgb[1], p->rgb[2]);
    else
        make_box(p);
    if (edits) {
        p->quiet = true;
        for (int i = 0; i < 3; i++) {
            char t[4];
            snprintf(t, sizeof(t), "%d", p->rgb[i]);
            ctl_set_text(p->edit[i], t);
        }
        p->quiet = false;
    }
    wm_invalidate(p->field->win);
}

static void from_hsv(pick_t *p) {
    hsv_rgb(p->hue, p->sat, p->val, p->rgb);
    show(p, true);
}

static void on_field(control *c, int ev) {
    pick_t *p = c->user;
    if (ev != EV_MOUSEDOWN && ev != EV_MOUSEMOVE && ev != EV_DBLCLICK)
        return;
    int x = c->mx < 0 ? 0 : c->mx >= FW ? FW - 1 : c->mx;
    int y = c->my < 0 ? 0 : c->my >= FH ? FH - 1 : c->my;
    p->hue = x * HUES / FW;
    p->sat = 255 - y * 255 / (FH - 1);
    if (p->val < 8) /* (black stays black whatever the hue: brighten) */
        p->val = 63;
    make_bar(p);
    from_hsv(p);
}

static void on_bar(control *c, int ev) {
    pick_t *p = c->user;
    if (ev != EV_MOUSEDOWN && ev != EV_MOUSEMOVE && ev != EV_DBLCLICK)
        return;
    int y = c->my < 0 ? 0 : c->my >= FH ? FH - 1 : c->my;
    p->val = 63 - y * 63 / (FH - 1);
    from_hsv(p);
}

static void on_edit(control *c, int ev) {
    pick_t *p = c->user;
    if (ev != EV_CHANGE || p->quiet)
        return;
    for (int i = 0; i < 3; i++) {
        int v = 0;
        for (const char *t = ctl_text(p->edit[i]); *t >= '0' && *t <= '9'; t++)
            v = v * 10 + (*t - '0');
        p->rgb[i] = (uint8_t)(v > 63 ? 63 : v);
    }
    rgb_hsv(p->rgb, &p->hue, &p->sat, &p->val);
    make_bar(p);
    show(p, false);
}

static void paint_basic(control *c, int x, int y, int w, int h) {
    (void)c;
    int cw = w / 16;
    for (int i = 0; i < 16; i++) {
        fill_rect(x + i * cw, y, cw - 1, h, BASIC[i]);
        draw_rect(x + i * cw, y, cw - 1, h, BLACK);
    }
}

static void on_basic(control *c, int ev) {
    pick_t *p = c->user;
    if (ev != EV_MOUSEDOWN && ev != EV_DBLCLICK)
        return;
    int i = c->mx / (c->aw / 16);
    if (i < 0 || i > 15)
        return;
    memcpy(p->rgb, p->pal[BASIC[i]], 3);
    rgb_hsv(p->rgb, &p->hue, &p->sat, &p->val);
    make_bar(p);
    show(p, true);
}

/* closed: the result to the caller (NULL: cancelled), the preview's
 * colour put back if it was cancelled */
static void done(window *form, bool ok) {
    pick_t *p = dlg_form_state(form);
    window *owner = p->owner;
    dlg_color_cb cb = p->cb;
    void *user = p->user;
    static uint8_t result[3];
    memcpy(result, p->rgb, 3);
    if (!ok && live(p))
        gpu_set_color(p->preview, p->old[0], p->old[1], p->old[2]);
    dlg_form_close(form); /* (frees p) */
    if (cb)
        ui_guard(owner, (void (*)(void *, void *))(void (*)(void))cb, user,
                 ok ? result : NULL, "the colour picker's answer");
}

static void on_ok(control *c, int ev) {
    (void)ev;
    done(c->win, true);
}
static void on_cancel(control *c, int ev) {
    (void)ev;
    done(c->win, false);
}
static bool on_close(window *w) {
    done(w, false);
    return true;
}

void dlg_color(window *owner, const char *title, const uint8_t rgb[3],
               int preview, dlg_color_cb cb, void *user) {
    window *w = dlg_form(owner, title ? title : "Colour", 232, 16 + 108 + 8,
                         sizeof(pick_t));
    if (!w)
        return;
    pick_t *p = dlg_form_state(w);
    w->on_close = on_close;
    p->owner = owner;
    p->cb = cb;
    p->user = user;
    int n = gpu_palette(p->pal, 256);
    p->n = n > GPU_CUSTOM ? GPU_CUSTOM : n; /* (not the changing ones) */
    p->preview =
        n >= 256 && preview >= GPU_CUSTOM && preview < 256 ? preview : -1;
    for (int i = 0; i < 3; i++)
        p->old[i] = p->rgb[i] = rgb ? rgb[i] & 63 : 63;
    rgb_hsv(p->rgb, &p->hue, &p->sat, &p->val);
    make_field(p);
    make_bar(p);
    make_box(p);

    p->field = ctl_custom(w, 6, 6, FW, FH, paint_field, on_field, 0);
    p->bar = ctl_custom(w, 6 + FW + 6, 6, BW, FH, paint_bar, on_bar, 0);
    p->box = ctl_custom(w, 172, 6, PW, PH, paint_box, NULL, CS_BORDER);
    ctl_set_tooltip(p->field, "Across: the hue. Down: greyer");
    ctl_set_tooltip(p->bar, "Lighter or darker");
    static const char *const RGB[3] = {"Red", "Green", "Blue"};
    for (int i = 0; i < 3; i++) {
        ctl_label(w, 172, 42 + i * 14, 26, 12, RGB[i], 0);
        p->edit[i] =
            ctl_edit(w, 200, 41 + i * 14, 24, 12, "", 2, ES_NUMBER, on_edit);
    }
    control *basic = ctl_custom(w, 6, 76, 160, 10, paint_basic, on_basic, 0);
    ctl_set_tooltip(basic, "The 16 basic colours");
    control *ok = ctl_button(w, 118, 94, 50, 13, "OK", 1, on_ok);
    control *ca = ctl_button(w, 174, 94, 50, 13, "Cancel", 2, on_cancel);
    ctl_style_add(ok, CS_DEFAULT);
    ctl_style_add(ca, CS_CANCEL);
    for (control *c = w->controls; c; c = c->next)
        c->user = p;
    show(p, true);
    ctl_focus(ok);
}
