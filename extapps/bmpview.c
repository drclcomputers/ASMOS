// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* BMP Viewer: a .BMP picture in a window, scrolled both ways when it is
 * bigger. In 256 colours where the screen has them (the screen's own
 * palette, without the 8 custom colours, which other apps change), else
 * in the 16; dithered unless View > Dithering is off. The picture is read
 * a slice at a time (on_frame), so a big one fills in and the desktop
 * keeps moving. */

#include "extapps/shim.h"

#define BG DARK_GRAY /* around the picture, and where it isn't read yet */
#define SLICE_MS 25  /* reading, each frame */
#define STEP 16      /* an arrow key moves this many pixels */
#define SB 8         /* a scroll bar's width (ui/controls.c) */
#define STATUS 11    /* the status line (ui/controls.c) */

typedef struct {
    window *win; /* must stay first */
    control *view, *vsb, *hsb, *status;
    bmp_info_t bi;
    uint8_t *pix; /* bi.w x bi.h screen colour indexes */
    bmp_reader_t *rd;
    int done;           /* rows read */
    int sx, sy, vw, vh; /* the top left shown; the view's size */
    bool dither, drag;
    int drag_mx, drag_my, drag_sx, drag_sy;
    uint8_t pal[256][3]; /* the screen's colours */
    int ncol;            /* how many the picture may use (16 or 248) */
    char path[FS_PATH_SZ], dir[FS_PATH_SZ], title[64], dither_label[24];
    char why[80]; /* why the picture isn't shown (or only part of it) */
} bmpview_state_t;

static void set_status(bmpview_state_t *s) {
    char b[120];
    const bmp_info_t *bi = &s->bi;
    if (!s->path[0]) {
        snprintf(b, sizeof(b), "No picture. File > Open...");
    } else if (!s->pix) {
        snprintf(b, sizeof(b), "%s", s->why);
    } else if (s->rd) {
        snprintf(b, sizeof(b), "Reading... %d%%", s->done * 100 / bi->h);
    } else {
        char kind[24];
        if (bi->bpp <= 8)
            snprintf(kind, sizeof(kind), "%d colours", bi->colors);
        else
            snprintf(kind, sizeof(kind), "%d-bit colour", bi->bpp);
        snprintf(b, sizeof(b), "%d x %d, %s. Shown in %d colours%s.%s%s", bi->w,
                 bi->h, kind, s->ncol == 16 ? 16 : 256,
                 s->dither ? ", dithered" : "", s->why[0] ? " " : "", s->why);
    }
    ctl_set_text(s->status, b);
}

/* keep the view inside the picture; the bars say where it is */
static void clamp_scroll(bmpview_state_t *s) {
    int mx = s->pix && s->bi.w > s->vw ? s->bi.w - s->vw : 0;
    int my = s->pix && s->bi.h > s->vh ? s->bi.h - s->vh : 0;
    s->sx = s->sx < 0 ? 0 : s->sx > mx ? mx : s->sx;
    s->sy = s->sy < 0 ? 0 : s->sy > my ? my : s->sy;
    ctl_set_range(s->hsb, 0, mx, s->vw > 0 ? s->vw : 1);
    ctl_set_value(s->hsb, s->sx);
    ctl_enable(s->hsb, mx > 0);
    ctl_set_range(s->vsb, 0, my, s->vh > 0 ? s->vh : 1);
    ctl_set_value(s->vsb, s->sy);
    ctl_enable(s->vsb, my > 0);
}

static void scroll_to(bmpview_state_t *s, int x, int y) {
    s->sx = x;
    s->sy = y;
    clamp_scroll(s);
    wm_invalidate(s->win);
}

static void drop_picture(bmpview_state_t *s) {
    if (s->rd)
        bmp_end(s->rd);
    s->rd = NULL;
    if (s->pix)
        kfree(s->pix);
    s->pix = NULL;
}

static void load(bmpview_state_t *s, const char *path) {
    drop_picture(s); /* first: the new one may need the room */
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->why[0] = 0;
    s->sx = s->sy = s->done = 0;
    const char *base = strrchr(s->path, '/');
    base = base ? base + 1 : s->path;
    snprintf(s->title, sizeof(s->title), "%s - BMP Viewer", base);
    s->win->title = s->title;
    strncpy(s->dir, s->path, sizeof(s->dir) - 1); /* for the next Open */
    char *sl = strrchr(s->dir, '/');
    if (sl)
        *(sl == s->dir ? sl + 1 : sl) = 0;

    const char *e = bmp_open(path, &s->bi);
    uint32_t size = e ? 0 : (uint32_t)s->bi.w * (uint32_t)s->bi.h;
    if (!e && !(s->pix = (uint8_t *)kmalloc(size))) {
        snprintf(s->why, sizeof(s->why),
                 "Not enough memory for it: %d x %d needs %u KB.", s->bi.w,
                 s->bi.h, (unsigned)(size / 1024 + 1));
    } else if (e) {
        snprintf(s->why, sizeof(s->why), "%s", e);
    } else {
        memset(s->pix, BG, size);
        bmp_target_t t = {(const uint8_t (*)[3])s->pal, s->ncol, 0, s->dither};
        s->rd = bmp_start(path, &s->bi, &t, s->pix, s->bi.w, s->bi.h, s->bi.w);
        if (!s->rd) {
            drop_picture(s);
            snprintf(s->why, sizeof(s->why), "Not enough memory to read it.");
        }
    }
    clamp_scroll(s);
    set_status(s);
    wm_invalidate(s->win);
}

// the view

static void paint_view(control *c, int x, int y, int w, int h) {
    bmpview_state_t *s = c->user;
    if (w != s->vw || h != s->vh) { /* the window was resized */
        s->vw = w;
        s->vh = h;
        clamp_scroll(s);
    }
    if (!s->pix) {
        fill_rect(x, y, w, h, BG);
        return;
    }
    int pw = s->bi.w, ph = s->bi.h;
    int dw = pw - s->sx < w ? pw - s->sx : w;
    int dh = ph - s->sy < h ? ph - s->sy : h;
    int ox = pw < w ? (w - pw) / 2 : 0, oy = ph < h ? (h - ph) / 2 : 0;
    if (ox || oy || dw < w || dh < h) { /* (smaller: in the middle) */
        fill_rect(x, y, w, oy, BG);
        fill_rect(x, y + oy + dh, w, h - oy - dh, BG);
        fill_rect(x, y + oy, ox, dh, BG);
        fill_rect(x + ox + dw, y + oy, w - ox - dw, dh, BG);
    }
    draw_pixels(x + ox, y + oy, dw, dh,
                s->pix + (uint32_t)s->sy * (uint32_t)pw + (uint32_t)s->sx, pw);
}

/* Edit > Copy (Ctrl+C): the picture as shown, in the screen's colours */
static void copy_picture(bmpview_state_t *s) {
    const char *e = "Copied: paste it in ASMDraw, or into a folder as a .BMP.";
    if (!s->pix || s->rd)
        e = "No picture to copy yet.";
    else if ((uint32_t)(s->bi.w * s->bi.h) > CLIP_IMAGE_MAX)
        e = "Too big to copy (the clipboard takes 320 KB).";
    else if (!clipboard_set_image(s->bi.w, s->bi.h, s->pix, s->bi.w,
                                  (const uint8_t (*)[3])s->pal))
        e = "Not enough memory to copy it.";
    ctl_set_text(s->status, e);
}

static void menu_copy(void) {
    bmpview_state_t *s = ui_active_state();
    if (s)
        copy_picture(s);
}

static void on_view(control *c, int ev) {
    bmpview_state_t *s = c->user;
    if (!s->pix)
        return;
    if (ev == EV_MOUSEDOWN || ev == EV_DBLCLICK) { /* drag to move it */
        s->drag = true;
        s->drag_mx = c->mx;
        s->drag_my = c->my;
        s->drag_sx = s->sx;
        s->drag_sy = s->sy;
    } else if (ev == EV_MOUSEMOVE && s->drag) {
        scroll_to(s, s->drag_sx - (c->mx - s->drag_mx),
                  s->drag_sy - (c->my - s->drag_my));
    } else if (ev == EV_MOUSEUP) {
        s->drag = false;
    } else if (ev == EV_KEY && kb.ctrl && kb.last_scancode == 0x2E) {
        copy_picture(s);
    } else if (ev == EV_KEY) {
        int x = s->sx, y = s->sy;
        int px = s->vw > STEP ? s->vw - STEP : STEP;
        int py = s->vh > STEP ? s->vh - STEP : STEP;
        switch (kb.last_scancode) {
        case LEFT_ARROW:
            x -= STEP;
            break;
        case RIGHT_ARROW:
            x += STEP;
            break;
        case UP_ARROW:
            y -= STEP;
            break;
        case DOWN_ARROW:
            y += STEP;
            break;
        case PAGE_UP:
            if (kb.ctrl)
                x -= px;
            else
                y -= py;
            break;
        case PAGE_DOWN:
            if (kb.ctrl)
                x += px;
            else
                y += py;
            break;
        case HOME:
            x = 0;
            if (kb.ctrl)
                y = 0;
            break;
        case END:
            x = s->bi.w;
            if (kb.ctrl)
                y = s->bi.h;
            break;
        default:
            return;
        }
        scroll_to(s, x, y);
    }
}

static void on_scroll(control *c, int ev) {
    bmpview_state_t *s = c->user;
    if (ev != EV_CHANGE)
        return;
    if (c == s->hsb)
        scroll_to(s, ctl_value(c), s->sy);
    else
        scroll_to(s, s->sx, ctl_value(c));
}

// menus

static void opened(void *user, const char *path) {
    bmpview_state_t *s = user;
    if (path)
        load(s, path);
}

static void menu_open(void) {
    bmpview_state_t *s = ui_active_state();
    if (s)
        dlg_open_file(s->win, "Open picture", s->dir, "BMP", opened, s);
}

static void dither_text(bmpview_state_t *s) {
    snprintf(s->dither_label, sizeof(s->dither_label), "Dithering: %s",
             s->dither ? "on" : "off");
}

static void menu_dither(void) {
    bmpview_state_t *s = ui_active_state();
    if (!s)
        return;
    s->dither = !s->dither;
    dither_text(s);
    if (s->path[0])
        load(s, s->path); /* again, the other way */
}

static void menu_close(void) {
    bmpview_state_t *s = ui_active_state();
    if (s)
        os_close_own_instance(s->win);
}

static void menu_about(void) {
    dlg_message(NULL, "About BMP Viewer",
                "BMP Viewer v1.1\nWindows and OS/2 .BMP pictures: 1, 4, 8, "
                "16, 24 and 32 bits, RLE too.\nDrag the picture or use the "
                "arrows, PgUp, PgDn (Ctrl: sideways), Home, End.\nEdit > Copy "
                "(Ctrl+C) puts it on the clipboard.");
}

static void bmpview_init(void *state) {
    bmpview_state_t *s = state;
    int sw = g_screen_width, sh = g_screen_height - MENUBAR_H;
    int w = sw * 3 / 4, h = sh * 3 / 4;
    s->win = app_window(s, "BMP Viewer", 16, 12, w, h, DARK_GRAY, AW_RESIZABLE);
    if (!s->win)
        return;
    s->win->lazy_custom = true; /* redrawn when it changes */
    s->win->min_w = 120;
    s->win->min_h = 80;
    int cx, cy, cw, ch;
    win_client_rect(s->win, &cx, &cy, &cw, &ch);
    s->view = ctl_custom(s->win, 0, 0, cw - SB, ch - STATUS - SB, paint_view,
                         on_view, CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);
    s->vsb = ctl_vscroll(s->win, cw - SB, 0, ch - STATUS - SB, on_scroll);
    ctl_style_add(s->vsb, CS_ANCHOR_RIGHT | CS_STRETCH_H);
    s->hsb = ctl_hscroll(s->win, 0, ch - STATUS - SB, cw - SB, on_scroll);
    ctl_style_add(s->hsb, CS_ANCHOR_BOTTOM | CS_STRETCH_W);
    s->status = ctl_status(s->win, "");
    s->vw = cw - SB;
    s->vh = ch - STATUS - SB;

    /* the screen's colours: 256 less the 8 custom ones, or 16 */
    int n = A->screen_palette(s->pal, 256);
    s->ncol = n > APP_COLOR_CUSTOM ? APP_COLOR_CUSTOM : n;
    s->dither = app_prefs_get_int("BMPVIEW", "dither", 1) != 0;
    if (!app_prefs_get("BMPVIEW", "dir", s->dir, sizeof(s->dir)))
        strcpy(s->dir, "/");
    dither_text(s);

    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "Open...", menu_open);
    menu_add_separator(f);
    menu_add_item(f, "Close", menu_close);
    menu_add_item(f, "About", menu_about);
    menu *ed = window_add_menu(s->win, "Edit");
    menu_add_item(ed, "Copy", menu_copy);
    menu *v = window_add_menu(s->win, "View");
    menu_add_item(v, s->dither_label, menu_dither);
    clamp_scroll(s);
    set_status(s);
    ctl_focus(s->view);
}

static void bmpview_frame(void *state) {
    bmpview_state_t *s = state;
    if (!s->win)
        return;
    char p[FS_PATH_SZ]; /* FileF: show this one (a new window each time) */
    if (app_open_request(p, sizeof(p)))
        load(s, p);
    if (!s->rd)
        return;
    uint32_t t0 = time_millis();
    int before = s->done;
    while (s->done < s->bi.h && !bmp_error(s->rd) &&
           time_millis() - t0 < SLICE_MS)
        s->done = bmp_rows(s->rd, 4);
    if (s->done >= s->bi.h || bmp_error(s->rd)) {
        if (bmp_error(s->rd))
            snprintf(s->why, sizeof(s->why), "%s", bmp_error(s->rd));
        bmp_end(s->rd);
        s->rd = NULL;
    }
    if (s->done != before || !s->rd) {
        set_status(s);
        wm_invalidate(s->win);
    }
}

static void bmpview_destroy(void *state) {
    bmpview_state_t *s = state;
    if (s->win) {
        app_prefs_begin("BMPVIEW");
        app_prefs_set_int("BMPVIEW", "dither", s->dither);
        app_prefs_set("BMPVIEW", "dir", s->dir);
        app_prefs_end();
    }
    drop_picture(s);
}

/* the picture, a byte a pixel: up to about 1 MB (1024 x 1024) */
APP_META("BMP Viewer", bmpview_state_t, false, 1104, bmpview_init,
         bmpview_frame, bmpview_destroy);
