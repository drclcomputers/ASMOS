// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* .BMP pictures (lib/bmp.c): BMP Viewer shows each kind as the
 * screen's colours (256 or 16: run with @640x256 too), scrolls a big one
 * both ways, says why it can't show a bad one; ASMDraw opens and saves
 * them and keeps its custom colours; the kernel's own calls read and save
 * them too (fixtures/bmp/mk.py makes the files) */
#include "scen.h"

static uint8_t s_buf[400000], s_exp[400000];
static int s_ew, s_eh;
static char s_kind;

/* fixtures/bmp/NAME.BMP to /NAME.BMP, and what it should look like */
static bool put(const char *name) {
    char p[64];
    uint32_t n = 0;
    snprintf(p, sizeof p, FIXDIR "bmp/%s.BMP", name);
    if (!load_file(p, s_buf, sizeof s_buf, &n))
        return false;
    fat_file_t f;
    snprintf(p, sizeof p, "/%s.BMP", name);
    PUT(p, s_buf, n);
    s_kind = 0;
    snprintf(p, sizeof p, FIXDIR "bmp/%s.EXP", name);
    if (load_file(p, s_exp, sizeof s_exp, &n) && n >= 5) {
        s_kind = (char)s_exp[0];
        s_ew = s_exp[1] | s_exp[2] << 8;
        s_eh = s_exp[3] | s_exp[4] << 8;
    }
    return true;
}

static control *nth(window *w, int type, int k) {
    for (control *c = w ? w->controls : NULL; c; c = c->next)
        if (c->type == type && k-- == 0)
            return c;
    return NULL;
}

static const char *status(window *w) {
    control *c = nth(w, CTL_STATUS, 0);
    return c && c->text ? c->text : "";
}

/* the viewer, with the picture read */
static window *view(const char *name) {
    char p[64];
    snprintf(p, sizeof p, "/%s.BMP", name);
    appload_run_named("BMP Viewer", p);
    sim_frames(2);
    window *w = wm_focused_window();
    for (int i = 0; i < 600 && w && !strncmp(status(w), "Reading", 7); i++)
        sim_frames(1);
    sim_frames(1);
    return w;
}

static bool run_menu(window *w, const char *prefix) {
    for (int m = 0; w && m < w->win_menu_count; m++)
        for (int k = 0; k < w->win_menus[m].item_count; k++) {
            menu_item *it = &w->win_menus[m].items[k];
            if (it->label && !strncmp(it->label, prefix, strlen(prefix))) {
                menu_item_run(it);
                return true;
            }
        }
    return false;
}

static uint8_t s_pal[256][3];

/* the screen's pixel for picture pixel (x, y), the view scrolled to (sx,
 * sy) and the picture in the middle when it is smaller */
static uint8_t shown(control *v, int pw, int ph, int sx, int sy, int x, int y) {
    int ox = pw < v->aw ? (v->aw - pw) / 2 : 0;
    int oy = ph < v->ah ? (v->ah - ph) / 2 : 0;
    int sxp = v->ax + ox + x - sx, syp = v->ay + oy + y - sy;
    return BACKBUF[syp * SCREEN_WIDTH + sxp];
}

static int b8(int v) { return (v << 2) | (v >> 4); }

/* how far a shown colour is from what the file has (8 bits) */
static int off_by(uint8_t idx, const uint8_t *rgb) {
    int d = 0;
    for (int k = 0; k < 3; k++) {
        int e = b8(s_pal[idx][k]) - rgb[k];
        e = e < 0 ? -e : e;
        d = e > d ? e : d;
    }
    return d;
}

/* every pixel: the colour index (E) or near enough (C, tol) */
static int bad_pixels(control *v, int tol) {
    int bad = 0;
    for (int y = 0; y < s_eh; y++)
        for (int x = 0; x < s_ew; x++) {
            uint8_t c = shown(v, s_ew, s_eh, 0, 0, x, y);
            if (s_kind == 'E')
                bad += c != s_exp[5 + y * s_ew + x];
            else
                bad += off_by(c, s_exp + 5 + (y * s_ew + x) * 3) > tol;
        }
    return bad;
}

/* the average colour of 8x8 blocks against the file's: how far off (the
 * part in the view: at 320x200 the picture may be bigger) */
static int block_error(control *v) {
    long sum = 0, n = 0;
    int h = s_eh < v->ah ? s_eh : v->ah, w = s_ew < v->aw ? s_ew : v->aw;
    for (int by = 0; by + 8 <= h; by += 8)
        for (int bx = 0; bx + 8 <= w; bx += 8)
            for (int k = 0; k < 3; k++) {
                long a = 0, b = 0;
                for (int y = by; y < by + 8; y++)
                    for (int x = bx; x < bx + 8; x++) {
                        a += b8(s_pal[shown(v, s_ew, s_eh, 0, 0, x, y)][k]);
                        b += s_exp[5 + (y * s_ew + x) * 3 + k];
                    }
                long d = (a - b) / 64;
                sum += d < 0 ? -d : d;
                n++;
            }
    return n ? (int)(sum / n) : 999;
}

static int viewer(void) {
    int fails = 0;
    char b[300];
    int ncol = gpu_palette(s_pal, 256);
    snprintf(b, sizeof b, "    the screen: %dx%d, %d colours\n", SCREEN_WIDTH,
             SCREEN_HEIGHT, ncol);
    wr(1, b);
    static const char *const exact[] = {"E8",  "E4", "E1", "ET", "E24",
                                        "E32", "R8", "R4", "O8"};
    for (unsigned i = 0; i < sizeof exact / sizeof exact[0]; i++) {
        if (!put(exact[i]) || s_kind != 'E') {
            T("the fixture is there (fixtures/make.sh)", false);
            continue;
        }
        window *w = view(exact[i]);
        control *v = nth(w, CTL_CUSTOM, 0);
        int bad = v ? bad_pixels(v, 0) : -1;
        snprintf(b, sizeof b, "    %s: %s; %d wrong pixels\n", exact[i],
                 status(w), bad);
        wr(1, b);
        snprintf(b, sizeof b, "%s: the screen's own colours, exactly",
                 exact[i]);
        char d[64];
        strncpy(d, b, sizeof d - 1);
        d[sizeof d - 1] = 0;
        T(d, bad == 0);
        if (i == 0) {
            sim_shot("bmp_e8");
            T("not bigger: no scrolling", !nth(w, CTL_HSCROLL, 0)->enabled &&
                                              !nth(w, CTL_VSCROLL, 0)->enabled);
        }
        wm_close(w);
        sim_frames(2);
    }

    /* 16 bits a pixel: not exactly the screen's colours (5-6-5 rounds
     * them), near them on average (dithering spreads what is left over) */
    put("E16");
    window *w = view("E16");
    control *v = nth(w, CTL_CUSTOM, 0);
    int bad = block_error(v);
    snprintf(b, sizeof b, "    E16: %s; off by %d on average\n", status(w),
             bad);
    wr(1, b);
    T("16-bit (5-6-5 masks): the right colours", bad <= 6);
    wm_close(w);
    sim_frames(2);

    /* a photo: dithered, its colours average out; nearest is worse */
    put("PHOTO");
    w = view("PHOTO");
    v = nth(w, CTL_CUSTOM, 0);
    int dith = block_error(v);
    sim_shot("bmp_photo");
    T("dithered: says so", strstr(status(w), "dithered") != NULL);
    T("the status says how many colours",
      strstr(status(w), ncol == 256 ? "Shown in 256" : "Shown in 16"));
    run_menu(w, "Dithering");
    for (int i = 0; i < 600 && !strncmp(status(w), "Reading", 7); i++)
        sim_frames(1);
    sim_frames(1);
    int plain = block_error(v);
    sim_shot("bmp_photo_plain");
    snprintf(b, sizeof b, "    photo: off by %d on average dithered, %d not\n",
             dith, plain);
    wr(1, b);
    T("dithered: close on average", dith <= (ncol == 256 ? 6 : 14));
    T("dithering helps (or ties)", dith <= plain);
    run_menu(w, "Dithering"); /* (back on, for the next tests) */
    sim_frames(3);
    wm_close(w);
    sim_frames(2);

    /* bigger than the window: both bars, the keys, dragging */
    put("BIG");
    w = view("BIG");
    v = nth(w, CTL_CUSTOM, 0);
    control *hs = nth(w, CTL_HSCROLL, 0), *vs = nth(w, CTL_VSCROLL, 0);
    snprintf(b, sizeof b, "    BIG: %s\n    view %dx%d, bars to %d and %d\n",
             status(w), v->aw, v->ah, hs->max, vs->max);
    wr(1, b);
    T("both bars, as far as the picture goes", hs->enabled && vs->enabled &&
                                                   hs->max == 700 - v->aw &&
                                                   vs->max == 450 - v->ah);
    T("the top left first",
      hs->value == 0 && vs->value == 0 &&
          shown(v, 700, 450, 0, 0, 5, 5) == s_exp[5 + 5 * 700 + 5]);
    sim_key2(0x4F, 0, false, true); /* Ctrl+End: the bottom right corner */
    sim_frames(2);
    int sx = hs->value, sy = vs->value;
    T("Ctrl+End: the bottom right", sx == hs->max && sy == vs->max);
    T("... shown there",
      shown(v, 700, 450, sx, sy, 699, 449) == s_exp[5 + 449 * 700 + 699] &&
          shown(v, 700, 450, sx, sy, 650, 420) == s_exp[5 + 420 * 700 + 650]);
    sim_shot("bmp_big_end");
    sim_drag(v->ax + 100, v->ay + 80, v->ax + 140, v->ay + 110);
    sim_frames(2);
    snprintf(b, sizeof b, "    dragged: %d,%d -> %d,%d\n", sx, sy, hs->value,
             vs->value);
    wr(1, b);
    T("dragging moves it", hs->value == sx - 40 && vs->value == sy - 30);
    sim_key2(0x47, 0, false, true); /* Ctrl+Home */
    sim_frames(2);
    T("Ctrl+Home: back to the top left", hs->value == 0 && vs->value == 0);
    sim_key(0x51); /* PgDn */
    sim_frames(2);
    T("PgDn: down a page", vs->value > 0 && vs->value < v->ah);
    wm_close(w);
    sim_frames(2);

    /* ones it can't show */
    put("NOT");
    w = view("NOT");
    T("not a picture: says so", strstr(status(w), "Not a BMP") != NULL);
    wm_close(w);
    sim_frames(2);
    put("CUT");
    w = view("CUT");
    snprintf(b, sizeof b, "    CUT: %s\n", status(w));
    wr(1, b);
    T("cut short: says so, shows what there is",
      strstr(status(w), "cut short") != NULL);
    wm_close(w);
    sim_frames(2);
    return fails;
}

// ASMDraw

/* File > Save As / Open: the name typed in the dialog, then Enter */
static void file_dialog(window *w, const char *item, const char *name) {
    run_menu(w, item);
    sim_frames(2);
    window *d = wm_focused_window();
    control *e = nth(d, CTL_EDIT, 0);
    if (e) { /* (Open starts in the list) */
        ctl_set_text(e, name);
        ctl_focus(e);
    }
    sim_key(0x1C);
    sim_frames(3);
    if (wm_focused_window() != w) { /* "Replace it?": yes */
        sim_key(0x1C);
        sim_frames(3);
    }
}

/* the canvas's pixel (x, y) on the screen */
static uint8_t canvas_px(control *cv, int x, int y) {
    return BACKBUF[(cv->ay + 2 + y) * SCREEN_WIDTH + cv->ax + 2 + x];
}

static bool line_is(control *cv, int y, int x0, int x1, uint8_t c) {
    for (int x = x0; x <= x1; x++)
        if (canvas_px(cv, x, y) != c)
            return false;
    return true;
}

static void slide(control *sl, int v) {
    ctl_set_value(sl, v);
    ctl_invoke(sl, EV_CHANGE);
}

static int draw_files(void) {
    int fails = 0;
    char b[300];
    bool many = gpu_palette(s_pal, 256) == 256;
    app_prefs_begin("ASMDRAW"); /* a clean start: the root, white customs */
    app_prefs_set("ASMDRAW", "dir", "/");
    app_prefs_set_int("ASMDRAW", "pen", 0);
    for (int k = 0; k < 8; k++) {
        char key[4] = {'c', (char)('0' + k), 0};
        app_prefs_set_int("ASMDRAW", key, 0x3FFFF);
    }
    app_prefs_end();
    sim_launch("ASMDRAW");
    sim_frames(3);
    window *w = wm_focused_window();
    control *cv = nth(w, CTL_CUSTOM, 0), *pal = nth(w, CTL_CUSTOM, 1);
    control *sl[3] = {nth(w, CTL_SLIDER, 0), nth(w, CTL_SLIDER, 1),
                      nth(w, CTL_SLIDER, 2)};
    T("the colour sliders only with 256 colours", (sl[2] != NULL) == many);
    int n = many ? 24 : 16, cw = (pal->aw - 20) / n;
    uint8_t pen = 4; /* red, or the third custom colour */
    if (many) {
        sim_click(pal->ax + 20 + 18 * cw + cw / 2, pal->ay + 5);
        sim_frames(1);
        T("a custom colour starts white", s_pal[250][0] == 63 &&
                                              ctl_value(sl[0]) == 63 &&
                                              ctl_value(sl[2]) == 63);
        slide(sl[0], 10);
        slide(sl[1], 40);
        slide(sl[2], 20);
        sim_frames(1);
        gpu_palette(s_pal, 256);
        T("the sliders change the screen's colour 250",
          s_pal[250][0] == 10 && s_pal[250][1] == 40 && s_pal[250][2] == 20);
        pen = 250;
    } else {
        sim_click(pal->ax + 20 + 4 * cw + cw / 2, pal->ay + 5);
    }
    sim_drag(cv->ax + 12, cv->ay + 12, cv->ax + 60, cv->ay + 12);
    sim_click(pal->ax + 20 + 8 * cw + cw / 2, pal->ay + 5); /* blue */
    sim_drag(cv->ax + 12, cv->ay + 20, cv->ax + 60, cv->ay + 20);
    sim_frames(2);
    sim_shot("draw_custom");
    T("drawn with the pen",
      line_is(cv, 10, 10, 58, pen) && line_is(cv, 18, 10, 58, BLUE));

    /* .BMP: 8 bits with a custom colour in it, else 4 */
    file_dialog(w, "Save As...", "T1.BMP");
    uint8_t h[64 + 1024];
    uint32_t got = 0;
    fat_file_t f;
    if (fs_open("/T1.BMP", &f)) {
        got = (uint32_t)fs_read(&f, h, sizeof h);
        fs_close(&f);
    }
    snprintf(b, sizeof b, "    T1.BMP: %u bytes read, %d bits, title %s\n", got,
             got > 28 ? h[28] : -1, w->title);
    wr(1, b);
    T("saved as a .BMP",
      got > 54 && h[0] == 'B' && h[1] == 'M' && h[28] == (many ? 8 : 4));
    if (fs_open("/T1.BMP", &f)) { /* (out/T1.BMP: for other programs) */
        int k = fs_read(&f, s_buf, sizeof s_buf);
        fs_close(&f);
        sim_save(OUTDIR "T1.BMP", s_buf, k > 0 ? (unsigned)k : 0);
    }
    if (many)
        T("its palette has the custom colour",
          h[54 + 250 * 4 + 2] == ((10 << 2) | (10 >> 4)) &&
              h[54 + 250 * 4 + 1] == ((40 << 2) | (40 >> 4)));
    if (many) { /* white again; the file brings it back */
        slide(sl[0], 63);
        slide(sl[1], 63);
        slide(sl[2], 63);
    }
    run_menu(w, "New");
    sim_frames(2);
    T("New: white", line_is(cv, 10, 10, 58, WHITE));
    file_dialog(w, "Open...", "T1.BMP");
    sim_frames(2);
    gpu_palette(s_pal, 256);
    T("the .BMP opened: the same pixels",
      line_is(cv, 10, 10, 58, pen) && line_is(cv, 18, 10, 58, BLUE));
    if (many)
        T("... and its custom colour",
          s_pal[250][0] == 10 && s_pal[250][1] == 40 && ctl_value(sl[1]) == 40);

    /* .PIC: the custom colours after the pixels */
    file_dialog(w, "Save As...", "T1.PIC");
    if (many) {
        slide(sl[0], 63);
        slide(sl[1], 63);
        slide(sl[2], 63);
    }
    run_menu(w, "New");
    sim_frames(2);
    file_dialog(w, "Open...", "T1.PIC");
    sim_frames(2);
    gpu_palette(s_pal, 256);
    T("the .PIC opened: the same pixels", line_is(cv, 10, 10, 58, pen));
    if (many)
        T("... and its custom colour", s_pal[250][1] == 40);

    /* someone else's .BMP (24 bits): the screen's colours, cut to the
     * canvas if bigger */
    put("E24");
    file_dialog(w, "Open...", "E24.BMP");
    sim_frames(2);
    int bad = 0;
    for (int y = 0; y < s_eh; y++)
        for (int x = 0; x < s_ew; x++)
            bad += canvas_px(cv, x, y) != s_exp[5 + y * s_ew + x];
    T("a 24-bit .BMP: its colours exactly", bad == 0);
    put("BIG");
    file_dialog(w, "Open...", "BIG.BMP");
    sim_frames(2);
    control *st = nth(w, CTL_STATUS, 0);
    snprintf(b, sizeof b, "    BIG: %s\n", st && st->text ? st->text : "");
    wr(1, b);
    T("bigger than the canvas: cut, and says so",
      st && st->text && strstr(st->text, "cut to the window") &&
          canvas_px(cv, 100, 50) == s_exp[5 + 50 * 700 + 100]);

    /* a .PIC from a 256-colour screen, on a 16-colour one: the nearest */
    if (!many) {
        uint8_t pic[4 + 6 + 4 + 24] = {3, 0, 2, 0, 250, 250, 15, 100, 1, 4};
        memcpy(pic + 10, "CUS8", 4);
        for (int k = 0; k < 8; k++) /* custom 2 (250): bright red */
            pic[14 + k * 3] = 63,
                         pic[15 + k * 3] = pic[16 + k * 3] = k == 2 ? 0 : 63;
        PUT("/T256.PIC", pic, sizeof pic);
        file_dialog(w, "Open...", "T256.PIC");
        sim_frames(2);
        snprintf(b, sizeof b, "    T256.PIC: %d %d %d / %d %d %d\n",
                 canvas_px(cv, 0, 0), canvas_px(cv, 1, 0), canvas_px(cv, 2, 0),
                 canvas_px(cv, 0, 1), canvas_px(cv, 1, 1), canvas_px(cv, 2, 1));
        wr(1, b);
        T("its colours become the nearest of the 16",
          canvas_px(cv, 0, 0) == 4 && canvas_px(cv, 1, 0) == 4 &&
              canvas_px(cv, 2, 0) == 15 && canvas_px(cv, 0, 1) < 16 &&
              canvas_px(cv, 1, 1) == 1 && canvas_px(cv, 2, 1) == 4);
    }
    wm_close(w);
    sim_frames(3);
    return fails;
}

// the kernel's own (lib/bmp.h): what a wallpaper or icons would use

static int kernel_lib(void) {
    int fails = 0;
    char b[300];
    static uint8_t out[64 * 64], back[64 * 64], all[256][3];
    static bmp_info_t bi;
    gpu_palette256(all, 256);
    static const char *const exact[] = {"E8",  "E4", "E1", "ET", "E24",
                                        "E32", "R8", "R4", "O8"};
    int bad16 = 0, bad256 = 0;
    for (unsigned i = 0; i < sizeof exact / sizeof exact[0]; i++) {
        char p[32];
        put(exact[i]);
        snprintf(p, sizeof p, "/%s.BMP", exact[i]);
        const char *e = bmp_open(p, &bi);
        for (int n = 16; !e && n <= 248; n += 232) {
            bmp_target_t t = {(const uint8_t (*)[3])all, n, 0, n == 16};
            memset(out, 0xEE, sizeof out);
            e = bmp_read_all(p, &bi, &t, out, 64, 64, 64);
            for (int y = 0; !e && y < s_eh; y++)
                for (int x = 0; x < s_ew; x++)
                    *(n == 16 ? &bad16 : &bad256) +=
                        out[y * 64 + x] != s_exp[5 + y * s_ew + x];
        }
        if (e) {
            snprintf(b, sizeof b, "    %s: %s\n", exact[i], e);
            wr(1, b);
            bad16++;
        }
    }
    snprintf(b, sizeof b, "    wrong: %d (16, dithered), %d (248)\n", bad16,
             bad256);
    wr(1, b);
    T("every kind, to 16 and to 248 colours", !bad16 && !bad256);

    /* saved and read back: the same indexes, 8 bits and 4 */
    for (int i = 0; i < 64 * 40; i++)
        out[i] = (uint8_t)(i * 7 + i / 64);
    const char *e =
        bmp_save("/K8.BMP", out, 50, 40, 64, (const uint8_t (*)[3])all, 256);
    bmp_target_t t = {(const uint8_t (*)[3])all, 248, 256, false};
    memset(back, 0, sizeof back);
    if (!e)
        e = bmp_open("/K8.BMP", &bi);
    if (!e)
        e = bmp_read_all("/K8.BMP", &bi, &t, back, 64, 64, 64);
    int diff = 0;
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 50; x++)
            diff += back[y * 64 + x] != out[y * 64 + x];
    T("8 bits: saved and read back the same",
      !e && bi.bpp == 8 && bi.w == 50 && !diff);
    for (int i = 0; i < 64 * 40; i++)
        out[i] &= 15;
    e = bmp_save("/K4.BMP", out, 33, 21, 64, (const uint8_t (*)[3])all, 16);
    if (!e)
        e = bmp_open("/K4.BMP", &bi);
    bmp_target_t t16 = {(const uint8_t (*)[3])all, 16, 16, false};
    if (!e)
        e = bmp_read_all("/K4.BMP", &bi, &t16, back, 64, 64, 64);
    diff = 0;
    for (int y = 0; y < 21; y++)
        for (int x = 0; x < 33; x++)
            diff += back[y * 64 + x] != out[y * 64 + x];
    T("4 bits (odd width): saved and read back the same",
      !e && bi.bpp == 4 && bi.w == 33 && !diff);
    put("NOT");
    T("not a picture: why", bmp_open("/NOT.BMP", &bi) != NULL);
    return fails;
}

int scen_bmp(const char *name) {
    if (!strcmp(name, "bmplib"))
        return kernel_lib();
    if (!strcmp(name, "bmpview"))
        return viewer();
    if (!strcmp(name, "drawbmp"))
        return draw_files();
    return -1;
}
