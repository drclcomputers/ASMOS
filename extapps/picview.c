// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* Picture Viewer (was BMP Viewer): a picture in a window, scrolled both
 * ways when it is bigger. BMP (lib/bmp.h: read a slice at a time, so a big
 * one fills in and the desktop keeps moving), GIF and JPEG (lib/image.h:
 * read whole, made smaller if there isn't memory for all of it), and
 * ASMDraw's .PIC. In 256 colours where the screen has them (its own
 * palette, without the 8 custom colours, which other apps change), else
 * in the 16; dithered unless View > Dithering is off. */

#include "extapps/shim.h"

#define BG DARK_GRAY /* around the picture, and where it isn't read yet */
#define SLICE_MS 25  /* reading, each frame */
#define STEP 16      /* an arrow key moves this many pixels */
#define SB 8         /* a scroll bar's width (ui/controls.c) */
#define STATUS 11    /* the status line (ui/controls.c) */
#define FILE_MAX (2048u * 1024u) /* a GIF or JPEG file, at most */
#define PIX_MAX (1280u * 1024u)  /* its pixels, at most (else smaller) */

typedef struct {
    window *win; /* must stay first */
    control *view, *vsb, *hsb, *status;
    bmp_info_t bi;
    int kind; /* IMAGE_BMP (read in slices), IMAGE_GIF, IMAGE_JPEG, 'p' .PIC */
    int w, h; /* as shown */
    int fw, fh;   /* the file's (a GIF, JPEG shown smaller) */
    uint8_t *pix; /* w x h screen colour indexes */
    bmp_reader_t *rd;
    int done;           /* rows read */
    int sx, sy, vw, vh; /* the top left shown; the view's size */
    bool dither, drag;
    int drag_mx, drag_my, drag_sx, drag_sy;
    uint8_t pal[256][3]; /* the screen's colours */
    int ncol;            /* how many the picture may use (16 or 248) */
    char path[FS_PATH_SZ], dir[FS_PATH_SZ], title[64], dither_label[24];
    char why[80]; /* why the picture isn't shown (or only part of it) */
} picview_state_t;

static void set_status(picview_state_t *s) {
    char b[120];
    const bmp_info_t *bi = &s->bi;
    if (!s->path[0]) {
        snprintf(b, sizeof(b), "No picture. File > Open...");
    } else if (!s->pix) {
        snprintf(b, sizeof(b), "%s", s->why);
    } else if (s->rd) {
        snprintf(b, sizeof(b), "Reading... %d%%", s->done * 100 / bi->h);
    } else if (s->kind != IMAGE_BMP) {
        char sz[40] = "";
        if (s->fw != s->w)
            snprintf(sz, sizeof(sz), " (shown at %d x %d)", s->w, s->h);
        snprintf(b, sizeof(b), "%d x %d%s, %s. Shown in %d colours%s.", s->fw,
                 s->fh, sz,
                 s->kind == IMAGE_GIF    ? "GIF"
                 : s->kind == IMAGE_JPEG ? "JPEG"
                                         : "ASMDraw picture",
                 s->ncol == 16 ? 16 : 256,
                 s->dither && s->kind != 'p' ? ", dithered" : "");
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
static void clamp_scroll(picview_state_t *s) {
    int mx = s->pix && s->w > s->vw ? s->w - s->vw : 0;
    int my = s->pix && s->h > s->vh ? s->h - s->vh : 0;
    s->sx = s->sx < 0 ? 0 : s->sx > mx ? mx : s->sx;
    s->sy = s->sy < 0 ? 0 : s->sy > my ? my : s->sy;
    ctl_set_range(s->hsb, 0, mx, s->vw > 0 ? s->vw : 1);
    ctl_set_value(s->hsb, s->sx);
    ctl_enable(s->hsb, mx > 0);
    ctl_set_range(s->vsb, 0, my, s->vh > 0 ? s->vh : 1);
    ctl_set_value(s->vsb, s->sy);
    ctl_enable(s->vsb, my > 0);
}

static void scroll_to(picview_state_t *s, int x, int y) {
    s->sx = x;
    s->sy = y;
    clamp_scroll(s);
    wm_invalidate(s->win);
}

static void drop_picture(picview_state_t *s) {
    if (s->rd)
        bmp_end(s->rd);
    s->rd = NULL;
    if (s->pix)
        kfree(s->pix);
    s->pix = NULL;
}

/* three quarters the size (the same shape); false: too small to go on */
static bool smaller(picview_state_t *s) {
    if (s->w < 8 && s->h < 8)
        return false;
    s->w = s->w * 3 / 4 > 0 ? s->w * 3 / 4 : 1;
    s->h = s->h * 3 / 4 > 0 ? s->h * 3 / 4 : 1;
    return true;
}

/* a GIF or JPEG: the file read whole, decoded (at a size there is room
 * for); .PIC: its pixels, in the colours they were drawn in */
static void load_whole(picview_state_t *s, const char *path) {
    app_dirent_t de;
    if (!A->file_info(path, &de)) {
        snprintf(s->why, sizeof(s->why), "File not found.");
        return;
    }
    if (de.size > FILE_MAX) {
        snprintf(s->why, sizeof(s->why), "Too big: files up to %u KB.",
                 (unsigned)(FILE_MAX / 1024));
        return;
    }
    uint8_t *b = (uint8_t *)kmalloc(de.size + 1);
    int n = b ? A->file_read(path, b, (int)de.size) : -1;
    if (n <= 0) {
        kfree(b);
        if (b)
            snprintf(s->why, sizeof(s->why), "Could not read it.");
        else
            snprintf(s->why, sizeof(s->why),
                     "Not enough memory to read it (%u KB).",
                     (unsigned)(de.size / 1024 + 1));
        return;
    }
    s->kind = image_kind(b, (uint32_t)n);
    const char *ext = strrchr(path, '.');
    if (!s->kind && n >= 4 && ext && !strcasecmp(ext, ".PIC")) {
        /* .PIC: width, height, the pixels */
        int w = b[0] | b[1] << 8, h = b[2] | b[3] << 8;
        if (w > 0 && h > 0 && (uint32_t)(4 + w * h) <= (uint32_t)n)
            s->kind = 'p';
    }
    if (s->kind == 'p') {
        s->fw = s->w = b[0] | b[1] << 8;
        s->fh = s->h = b[2] | b[3] << 8;
        s->pix = (uint8_t *)kmalloc((uint32_t)(s->w * s->h));
        if (s->pix) {
            /* its indexes are the 256-colour palette's (and 8 colours of
             * its own at the end): the nearest the screen has */
            uint8_t all[256][3], map[256];
            A->palette256(all, 256);
            uint32_t tail = 4 + (uint32_t)(s->w * s->h);
            if (tail + 4 + 24 <= (uint32_t)n && !memcmp(b + tail, "CUS8", 4))
                memcpy(all[APP_COLOR_CUSTOM], b + tail + 4, 24);
            for (int i = 0; i < 256; i++) {
                int best = 0, bd = 0x7FFFFFFF;
                for (int k = 0; k < s->ncol; k++) {
                    int dr = all[i][0] - s->pal[k][0],
                        dg = all[i][1] - s->pal[k][1],
                        db = all[i][2] - s->pal[k][2];
                    int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                    if (d < bd) {
                        bd = d;
                        best = k;
                    }
                }
                map[i] = (uint8_t)best;
            }
            for (int i = 0; i < s->w * s->h; i++)
                s->pix[i] = map[b[4 + i]];
        }
    } else if (!s->kind) {
        snprintf(s->why, sizeof(s->why),
                 "Not a picture it knows (BMP, GIF, JPEG, PIC).");
    } else {
        const char *e = image_info(b, (uint32_t)n, &s->fw, &s->fh);
        if (e) {
            snprintf(s->why, sizeof(s->why), "%s", e);
            kfree(b);
            return;
        }
        /* all of it, or smaller (the same shape) until there is room for
         * it and for what the decoder works in */
        s->w = s->fw;
        s->h = s->fh;
        while ((uint32_t)s->w * (uint32_t)s->h > PIX_MAX)
            smaller(s);
        image_target_t t = {(const uint8_t (*)[3])s->pal,
                            s->ncol,
                            s->dither,
                            {96, 96, 96},
                            NULL,
                            NULL};
        for (;;) {
            s->pix = (uint8_t *)kmalloc((uint32_t)(s->w * s->h));
            if (s->pix) {
                e = image_decode(b, (uint32_t)n, s->pix, s->w, s->h, s->w, &t);
                if (!e)
                    break;
                kfree(s->pix);
                s->pix = NULL;
                if (strcmp(e, "Not enough memory.")) {
                    snprintf(s->why, sizeof(s->why), "%s", e);
                    break;
                }
            }
            if (!smaller(s))
                break;
        }
    }
    kfree(b);
    if (!s->pix && !s->why[0])
        snprintf(s->why, sizeof(s->why), "Not enough memory for it.");
}

static void load(picview_state_t *s, const char *path) {
    drop_picture(s); /* first: the new one may need the room */
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->why[0] = 0;
    s->sx = s->sy = s->done = 0;
    const char *base = strrchr(s->path, '/');
    base = base ? base + 1 : s->path;
    snprintf(s->title, sizeof(s->title), "%s - Picture Viewer", base);
    s->win->title = s->title;
    strncpy(s->dir, s->path, sizeof(s->dir) - 1); /* for the next Open */
    char *sl = strrchr(s->dir, '/');
    if (sl)
        *(sl == s->dir ? sl + 1 : sl) = 0;

    s->kind = 0;
    uint8_t magic[2] = {0, 0};
    A->file_read_at(path, 0, magic, 2);
    if (magic[0] != 'B' || magic[1] != 'M') { /* not a BMP: read whole */
        load_whole(s, path);
        clamp_scroll(s);
        set_status(s);
        wm_invalidate(s->win);
        return;
    }
    s->kind = IMAGE_BMP;
    const char *e = bmp_open(path, &s->bi);
    s->w = s->fw = e ? 0 : s->bi.w;
    s->h = s->fh = e ? 0 : s->bi.h;
    uint32_t size = e ? 0 : (uint32_t)s->w * (uint32_t)s->h;
    if (!e && !(s->pix = (uint8_t *)kmalloc(size))) {
        snprintf(s->why, sizeof(s->why),
                 "Not enough memory for it: %d x %d needs %u KB.", s->w, s->h,
                 (unsigned)(size / 1024 + 1));
    } else if (e) {
        snprintf(s->why, sizeof(s->why), "%s", e);
    } else {
        memset(s->pix, BG, size);
        bmp_target_t t = {(const uint8_t (*)[3])s->pal, s->ncol, 0, s->dither};
        s->rd = bmp_start(path, &s->bi, &t, s->pix, s->w, s->h, s->w);
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
    picview_state_t *s = c->user;
    if (w != s->vw || h != s->vh) { /* the window was resized */
        s->vw = w;
        s->vh = h;
        clamp_scroll(s);
    }
    if (!s->pix) {
        fill_rect(x, y, w, h, BG);
        return;
    }
    int pw = s->w, ph = s->h;
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
static void copy_picture(picview_state_t *s) {
    const char *e = "Copied: paste it in ASMDraw, or into a folder as a .BMP.";
    if (!s->pix || s->rd)
        e = "No picture to copy yet.";
    else if ((uint32_t)(s->w * s->h) > CLIP_IMAGE_MAX)
        e = "Too big to copy (the clipboard takes 320 KB).";
    else if (!clipboard_set_image(s->w, s->h, s->pix, s->w,
                                  (const uint8_t (*)[3])s->pal))
        e = "Not enough memory to copy it.";
    ctl_set_text(s->status, e);
}

static void menu_copy(void) {
    picview_state_t *s = ui_active_state();
    if (s)
        copy_picture(s);
}

static void on_view(control *c, int ev) {
    picview_state_t *s = c->user;
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
            x = s->w;
            if (kb.ctrl)
                y = s->h;
            break;
        default:
            return;
        }
        scroll_to(s, x, y);
    }
}

static void on_scroll(control *c, int ev) {
    picview_state_t *s = c->user;
    if (ev != EV_CHANGE)
        return;
    if (c == s->hsb)
        scroll_to(s, ctl_value(c), s->sy);
    else
        scroll_to(s, s->sx, ctl_value(c));
}

// menus

static void opened(void *user, const char *path) {
    picview_state_t *s = user;
    if (path)
        load(s, path);
}

static void menu_open(void) {
    picview_state_t *s = ui_active_state();
    if (s)
        dlg_open_file(s->win, "Open picture", s->dir,
                      "BMP,GIF,JPG,JPEG,JPE,PIC", opened, s);
}

static void dither_text(picview_state_t *s) {
    snprintf(s->dither_label, sizeof(s->dither_label), "Dithering: %s",
             s->dither ? "on" : "off");
}

static void menu_dither(void) {
    picview_state_t *s = ui_active_state();
    if (!s)
        return;
    s->dither = !s->dither;
    dither_text(s);
    if (s->path[0])
        load(s, s->path); /* again, the other way */
}

static void menu_close(void) {
    picview_state_t *s = ui_active_state();
    if (s)
        os_close_own_instance(s->win);
}

static void menu_about(void) {
    dlg_message(NULL, "About Picture Viewer",
                "Picture Viewer v2.0\nBMP (1 to 32 bits, RLE too), GIF, JPEG "
                "(not progressive) and ASMDraw's .PIC.\nDrag the picture or "
                "use the arrows, PgUp, PgDn (Ctrl: sideways), Home, End.\n"
                "Edit > Copy (Ctrl+C) puts it on the clipboard.");
}

static void picview_init(void *state) {
    picview_state_t *s = state;
    int sw = g_screen_width, sh = g_screen_height - MENUBAR_H;
    int w = sw * 3 / 4, h = sh * 3 / 4;
    s->win =
        app_window(s, "Picture Viewer", 16, 12, w, h, LIGHT_GRAY, AW_RESIZABLE);
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
    s->dither = app_prefs_get_int("PICVIEW", "dither", 1) != 0;
    if (!app_prefs_get("PICVIEW", "dir", s->dir, sizeof(s->dir)))
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

static void picview_frame(void *state) {
    picview_state_t *s = state;
    if (!s->win)
        return;
    char p[FS_PATH_SZ]; /* FileF: show this one (a new window each time) */
    if (app_open_request(p, sizeof(p)))
        load(s, p);
    if (!s->rd)
        return;
    uint32_t t0 = time_millis();
    int before = s->done;
    while (s->done < s->h && !bmp_error(s->rd) && time_millis() - t0 < SLICE_MS)
        s->done = bmp_rows(s->rd, 4);
    if (s->done >= s->h || bmp_error(s->rd)) {
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

static void picview_destroy(void *state) {
    picview_state_t *s = state;
    if (s->win) {
        app_prefs_begin("PICVIEW");
        app_prefs_set_int("PICVIEW", "dither", s->dither);
        app_prefs_set("PICVIEW", "dir", s->dir);
        app_prefs_end();
    }
    drop_picture(s);
}

APP_META("Picture Viewer", picview_state_t, false, 2048, picview_init,
         picview_frame, picview_destroy);
