// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* .BMP pictures (lib/bmp.c): BMP Viewer shows each kind as the
 * screen's colours (256 or 16: run with @640x256 too), scrolls a big one
 * both ways, says why it can't show a bad one; ASMDraw opens and saves
 * them and keeps its custom colours; the kernel's own calls read and save
 * them too (fixtures/bmp/mk.py makes the files) */
#include "lib/bmp.h"
#include "scen.h"
#include "ui/cursor.h"

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

/* a control by the start of its tooltip */
static control *tipped(window *w, const char *tip) {
    for (control *c = w ? w->controls : NULL; c; c = c->next)
        if (c->tip && !strncmp(c->tip, tip, strlen(tip)))
            return c;
    return NULL;
}

/* the biggest custom control: the canvas */
static control *canvas_of(window *w) {
    control *best = NULL;
    for (control *c = w ? w->controls : NULL; c; c = c->next)
        if (c->type == CTL_CUSTOM &&
            (!best || c->aw * c->ah > best->aw * best->ah))
            best = c;
    return best;
}

/* the colour picker (ui/colorpick.c): these numbers typed in */
static window *pick_rgb(int r, int g, int b) {
    window *d = wm_focused_window();
    int v[3] = {r, g, b};
    for (int i = 0; i < 3; i++) {
        control *e = nth(d, CTL_EDIT, i);
        char t[4];
        snprintf(t, sizeof t, "%d", v[i]);
        if (e) {
            ctl_set_text(e, t);
            ctl_invoke(e, EV_CHANGE);
        }
    }
    sim_frames(1);
    return d;
}

static void press(window *d, const char *label) {
    for (control *c = d ? d->controls : NULL; c; c = c->next)
        if (c->type == CTL_BUTTON && c->text && !strcmp(c->text, label)) {
            sim_click(c->ax + 5, c->ay + 5);
            break;
        }
    sim_frames(2);
}

/* a custom colour through Edit... and the picker */
static void set_custom(control *edit, int r, int g, int b) {
    sim_click(edit->ax + 5, edit->ay + 5);
    sim_frames(2);
    press(pick_rgb(r, g, b), "OK");
}

static void clean_prefs(void) { /* the root, white customs, the pencil */
    app_prefs_begin("ASMDRAW");
    app_prefs_set("ASMDRAW", "dir", "/");
    app_prefs_set_int("ASMDRAW", "pen", 0);
    app_prefs_set_int("ASMDRAW", "tool", 0);
    app_prefs_set_int("ASMDRAW", "size", 0);
    for (int k = 0; k < 8; k++) {
        char key[4] = {'c', (char)('0' + k), 0};
        app_prefs_set_int("ASMDRAW", key, 0x3FFFF);
    }
    app_prefs_end();
}

static int draw_files(void) {
    int fails = 0;
    char b[300];
    bool many = gpu_palette(s_pal, 256) == 256;
    clean_prefs();
    sim_launch("ASMDRAW");
    sim_frames(3);
    window *w = wm_focused_window();
    control *cv = canvas_of(w), *pal = tipped(w, "Click: the pen");
    control *edit = nth(w, CTL_BUTTON, 0);
    T("Edit... (the colour picker) only with 256 colours",
      (edit != NULL && !strcmp(edit->text, "Edit...")) == many);
    T("no sliders any more", nth(w, CTL_SLIDER, 0) == NULL);
    int n = many ? 24 : 16, cw = pal->aw / n;
    uint8_t pen = 4; /* red, or the third custom colour */
    if (many) {
        sim_click(pal->ax + 18 * cw + cw / 2, pal->ay + 5);
        sim_frames(1);
        T("a custom colour starts white", s_pal[250][0] == 63);
        sim_click(edit->ax + 5, edit->ay + 5); /* Edit... */
        sim_frames(2);
        window *d = pick_rgb(10, 40, 20);
        sim_shot("draw_picker");
        gpu_palette(s_pal, 256);
        T("the picker shows it at once, in colour 250",
          d != w && s_pal[250][0] == 10 && s_pal[250][1] == 40 &&
              s_pal[250][2] == 20);
        press(d, "OK");
        T("OK: kept", wm_focused_window() == w && s_pal[250][1] == 40);
        sim_click(edit->ax + 5, edit->ay + 5);
        sim_frames(2);
        press(pick_rgb(0, 0, 0), "Cancel");
        gpu_palette(s_pal, 256);
        T("Cancel: put back",
          s_pal[250][0] == 10 && s_pal[250][1] == 40 && s_pal[250][2] == 20);
        pen = 250;
    } else {
        sim_click(pal->ax + 4 * cw + cw / 2, pal->ay + 5);
    }
    sim_drag(cv->ax + 12, cv->ay + 12, cv->ax + 60, cv->ay + 12);
    sim_click(pal->ax + 8 * cw + cw / 2, pal->ay + 5); /* blue */
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
        set_custom(edit, 63, 63, 63);
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
          s_pal[250][0] == 10 && s_pal[250][1] == 40);

    /* .PIC: the custom colours after the pixels */
    file_dialog(w, "Save As...", "T1.PIC");
    if (many) {
        set_custom(edit, 63, 63, 63);
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

    /* Save As's Type list: the extension it adds, and the one it changes */
    run_menu(w, "New"); /* (no file: .PIC first) */
    sim_frames(2);
    fs_delete("/T2.BMP");
    fs_delete("/T3.PIC");
    run_menu(w, "Save As...");
    sim_frames(2);
    window *d = wm_focused_window();
    control *ty = nth(d, CTL_COMBO, 0), *e = nth(d, CTL_EDIT, 0);
    snprintf(b, sizeof b, "    Type: %s / %s\n", ty ? list_get(ty, 0) : "-",
             ty ? list_get(ty, 1) : "-");
    wr(1, b);
    T("Save As has a Type list: Picture first, then Bitmap",
      ty && list_count(ty) == 2 && strstr(list_get(ty, 0), "(.PIC)") &&
          strstr(list_get(ty, 1), "(.BMP)"));
    if (ty && e) {
        list_set_sel(ty, 1); /* Bitmap */
        ctl_invoke(ty, EV_CHANGE);
        ctl_set_text(e, "T2");
        sim_frames(1);
        sim_shot("save_types");
        ctl_focus(e);
        sim_key(0x1C);
        sim_frames(3);
    }
    dir_entry_t de;
    T("a name without one: the type's extension (T2.BMP)",
      fs_find("/T2.BMP", &de) && !fs_find("/T2.PIC", &de));
    run_menu(w, "Save As...");
    sim_frames(2);
    d = wm_focused_window();
    ty = nth(d, CTL_COMBO, 0), e = nth(d, CTL_EDIT, 0);
    T("... and now Bitmap first (the picture's type)",
      ty && strstr(list_get(ty, 0), "(.BMP)"));
    if (ty && e) {
        ctl_set_text(e, "T3.BMP");
        list_set_sel(ty, 1); /* Picture: the name follows */
        ctl_invoke(ty, EV_CHANGE);
        snprintf(b, sizeof b, "    the name now: %s\n", ctl_text(e));
        wr(1, b);
        T("another type: the typed name's extension changes",
          !strcmp(ctl_text(e), "T3.PIC"));
        ctl_focus(e);
        sim_key(0x1C);
        sim_frames(3);
    }
    T("... saved so", fs_find("/T3.PIC", &de));

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

/* the tools, each checked in the picture */
static int draw_tools(void) {
    int fails = 0;
    char b[300];
    clean_prefs();
    sim_launch("ASMDRAW");
    sim_frames(3);
    window *w = wm_focused_window();
    control *cv = canvas_of(w), *pal = tipped(w, "Click: the pen");
    int cw = pal->aw / (gpu_palette(s_pal, 256) == 256 ? 24 : 16);
#define TOOL(t)                                                                \
    do {                                                                       \
        control *t_ = tipped(w, t);                                            \
        if (t_)                                                                \
            sim_click(t_->ax + 5, t_->ay + 5);                                 \
        T("the button: " t, t_ != NULL);                                       \
    } while (0)
#define COLOR(i) sim_click(pal->ax + (i) * cw + cw / 2, pal->ay + 5)
#define AT(x, y) canvas_px(cv, x, y)
    int x0 = cv->ax + 2, y0 = cv->ay + 2; /* the canvas's (0, 0) */
    T("10 tools, 6 sizes", tipped(w, "Spray") && tipped(w, "Pick a colour") &&
                               tipped(w, "Brush: 1 pixel") &&
                               tipped(w, "Brush: 9 pixels"));

    /* a filled rectangle */
    COLOR(4); /* red */
    TOOL("Filled rectangle");
    sim_drag(x0 + 10, y0 + 10, x0 + 40, y0 + 30);
    sim_frames(1);
    T("filled rectangle: all of it", AT(10, 10) == 4 && AT(25, 20) == 4 &&
                                         AT(40, 30) == 4 && AT(41, 30) == 15 &&
                                         AT(25, 31) == 15);

    /* Shift + ellipse: a circle in the box 60..90 x 10..40 */
    COLOR(8); /* blue */
    TOOL("Ellipse");
    kb.shift = true;
    sim_drag(x0 + 60, y0 + 10, x0 + 90, y0 + 30); /* (wider than tall) */
    kb.shift = false;
    sim_frames(1);
    snprintf(b, sizeof b,
             "    circle: top %d bottom %d left %d right %d "
             "middle %d\n",
             AT(75, 10), AT(75, 40), AT(60, 25), AT(90, 25), AT(75, 25));
    wr(1, b);
    T("Shift: a circle (as tall as wide)",
      AT(75, 10) == 1 && AT(75, 40) == 1 && AT(60, 25) == 1 &&
          AT(90, 25) == 1 && AT(75, 25) == 15 && AT(75, 41) == 15);

    /* fill inside it */
    COLOR(11); /* yellow */
    TOOL("Fill");
    sim_click(x0 + 75, y0 + 25);
    sim_frames(1);
    T("fill: inside the circle, not out",
      AT(75, 25) == 14 && AT(66, 25) == 14 && AT(75, 10) == 1 &&
          AT(55, 25) == 15);

    /* a filled ellipse, and the colour picked back from it */
    COLOR(6); /* green */
    TOOL("Filled ellipse");
    sim_drag(x0 + 100, y0 + 10, x0 + 140, y0 + 30);
    sim_frames(1);
    T("filled ellipse: the middle, not the corner",
      AT(120, 20) == 2 && AT(101, 20) == 2 && AT(100, 10) == 15);
    COLOR(0); /* black */
    TOOL("Pick a colour");
    sim_click(x0 + 20, y0 + 20); /* the red rectangle */
    sim_frames(1);
    control *pens = tipped(w, "The pen over");
    T("pick: the pen's colour is red now, the tool back to the ellipse",
      BACKBUF[(pens->ay + 3) * SCREEN_WIDTH + pens->ax + 3] == 4);
    sim_drag(x0 + 150, y0 + 10, x0 + 170, y0 + 30);
    sim_frames(1);
    T("... (it draws a red filled ellipse)", AT(160, 20) == 4);

    /* brush sizes: a 9-pixel dot is 9 across, a line at 45 degrees */
    COLOR(0);
    TOOL("Pencil");
    TOOL("Brush: 9 pixels");
    sim_click(x0 + 20, y0 + 60);
    sim_frames(1);
    T("a 9-pixel brush: 9 across", AT(16, 60) == 0 && AT(24, 60) == 0 &&
                                       AT(15, 60) == 15 && AT(25, 60) == 15 &&
                                       AT(20, 56) == 0 && AT(20, 55) == 15);
    TOOL("Line");
    TOOL("Brush: 1 pixel");
    kb.shift = true;
    sim_drag(x0 + 40, y0 + 50, x0 + 70, y0 + 75); /* -> 45 degrees */
    kb.shift = false;
    sim_frames(1);
    T("Shift + line: 45 degrees", AT(40, 50) == 0 && AT(55, 65) == 0 &&
                                      AT(70, 80) == 0 && AT(70, 75) == 15);

    /* spray: dots near where it is held, none far */
    COLOR(5); /* light red: (palette order) */
    TOOL("Spray");
    TOOL("Brush: 3 pixels");
    sim_move(x0 + 120, y0 + 70);
    mouse.left = true;
    sim_frames(20);
    mouse.left = false;
    sim_frame();
    int in = 0, out = 0, r = 3 * 2 + 3;
    for (int y = 50; y < 90; y++)
        for (int x = 100; x < 140; x++) {
            int dx = x - 120, dy = y - 70;
            if (AT(x, y) == 12)
                *(dx * dx + dy * dy <= r * r ? &in : &out) += 1;
        }
    snprintf(b, sizeof b, "    spray: %d dots inside, %d outside\n", in, out);
    wr(1, b);
    T("spray: dots around it, while held", in > 20 && out == 0);
    sim_shot("draw_tools");
    wm_close(w);
    sim_frames(3);
#undef TOOL
#undef COLOR
#undef AT
    return fails;
}

// the selection in ASMDraw, the clipboard's pictures, the scrapbook

/* Ctrl + a key, to the focused control */
static void ctrl_key(uint8_t sc) { sim_key2(sc, 0, false, true); }

/* a key then Enter in the open pop-up menu: the item by its first letter */
static void popup_pick(char letter, uint8_t sc) {
    sim_key2(sc, letter, false, false);
    sim_key(0x1C);
    sim_frames(2);
}

static int draw_select(void) {
    int fails = 0;
    char b[300];
    clean_prefs();
    clipboard_history_clear();
    sim_launch("ASMDRAW");
    sim_frames(3);
    window *w = wm_focused_window();
    control *cv = canvas_of(w), *pal = tipped(w, "Click: the pen");
    int cw = pal->aw / (gpu_palette(s_pal, 256) == 256 ? 24 : 16);
#define TOOL(t)                                                                \
    do {                                                                       \
        control *t_ = tipped(w, t);                                            \
        if (t_)                                                                \
            sim_click(t_->ax + 5, t_->ay + 5);                                 \
    } while (0)
#define AT(x, y) canvas_px(cv, x, y)
    int x0 = cv->ax + 2, y0 = cv->ay + 2;
    T("the Select tool", tipped(w, "Select: drag a box") != NULL);
    sim_click(pal->ax + 4 * cw + cw / 2, pal->ay + 5); /* red */
    TOOL("Filled rectangle");
    sim_drag(x0 + 10, y0 + 10, x0 + 40, y0 + 30);

    /* a box 5..45 x 5..35 around it, copied */
    TOOL("Select: drag a box");
    sim_drag(x0 + 5, y0 + 5, x0 + 45, y0 + 35);
    sim_frames(1);
    ctrl_key(0x2E);
    const clip_image_t *im = clipboard_image();
    snprintf(b, sizeof b, "    copied: %d x %d\n", im ? im->w : -1,
             im ? im->h : -1);
    wr(1, b);
    T("Ctrl+C: the box on the clipboard, a picture",
      im && im->w == 41 && im->h == 31 && clipboard_kind() == CLIP_IMAGE);
    T("... its pixels", im && im->pix[5 * 41 + 5] == 4 &&
                            im->pix[25 * 41 + 35] == 4 && im->pix[0] == 15 &&
                            im->pix[30 * 41 + 40] == 15);
    T("... and colours", im && !memcmp(im->pal[4], s_pal[4], 3));
    T("in the scrapbook", clipboard_history_count() == 1 &&
                              clipboard_history_kind(0) == CLIP_IMAGE);

    /* over it: the move pointer; dragged: it moves, paper left behind */
    sim_move(x0 + 20, y0 + 20);
    sim_frames(2);
    T("over the selection: the move pointer", cursor_shown() == CURSOR_MOVE);
    sim_move(x0 + 100, y0 + 60);
    sim_frames(2);
    T("... not off it", cursor_shown() != CURSOR_MOVE);
    sim_drag(x0 + 20, y0 + 20, x0 + 80, y0 + 20);
    sim_frames(2);
    sim_shot("draw_select");
    T("moved: the paper where it was", AT(10, 10) == 15 && AT(25, 20) == 15);
    T("... the pixels where it went",
      AT(70, 10) == 4 && AT(100, 30) == 4 && AT(101, 31) == 15);

    /* right-click on it: Delete */
    sim_rclick(x0 + 85, y0 + 20);
    sim_frames(1);
    T("right-click: a menu", popup_is_open());
    popup_pick('d', 0x20);
    T("Delete: gone", !popup_is_open() && AT(70, 10) == 15 &&
                          AT(85, 20) == 15 && AT(100, 30) == 15);

    /* Ctrl+V: floats at the top left; a click elsewhere puts it down */
    ctrl_key(0x2F);
    sim_frames(1);
    T("Ctrl+V: the picture at the top left",
      AT(5, 5) == 4 && AT(35, 25) == 4 && AT(36, 26) == 15);
    sim_click(x0 + 120, y0 + 70);
    sim_frames(1);
    T("put down: it stays", AT(5, 5) == 4 && AT(35, 25) == 4);

    /* right-click elsewhere: Paste there */
    sim_rclick(x0 + 60, y0 + 50);
    sim_frames(1);
    popup_pick('p', 0x19);
    sim_key(0x01); /* Esc: put down */
    sim_frames(1);
    T("right-click, Paste: there", AT(65, 55) == 4 && AT(64, 54) == 15);

    /* Cut: Ctrl+A, Ctrl+X: an empty canvas, the whole of it copied */
    ctrl_key(0x1E);
    ctrl_key(0x2D);
    sim_frames(1);
    im = clipboard_image();
    T("Ctrl+A, Ctrl+X: all of it cut", im && im->w > 100 && AT(5, 5) == 15 &&
                                           AT(65, 55) == 15 &&
                                           clipboard_history_count() == 2);
    wm_close(w);
    sim_frames(3);
    if (wm_focused_window() && wm_focused_window() != w) { /* "Save?": no */
        press(wm_focused_window(), "No");
        sim_frames(2);
    }
#undef TOOL
#undef AT
    return fails;
}

static window *clipview(void) {
    sim_launch("ClipView");
    sim_frames(3);
    return FIND("ClipView");
}

static bool bmp_is(const char *path, int w, int h) {
    bmp_info_t bi;
    return !bmp_open(path, &bi) && bi.w == w && bi.h == h;
}

static int scrapbook(void) {
    int fails = 0;
    char b[300];
    gpu_palette(s_pal, 256);
    static uint8_t pix[40 * 30];
    for (int i = 0; i < 40 * 30; i++)
        pix[i] = i % 40 < 20 ? 4 : 1; /* red | blue */

    /* the scrapbook: text and pictures, the last 20, newest last */
    clipboard_history_clear();
    clipboard_set_text("one", (int)strlen("one"));
    clipboard_set_text("two", (int)strlen("two"));
    clipboard_set_image(40, 30, pix, 40, (const uint8_t (*)[3])s_pal);
    T("text, text, a picture", clipboard_history_count() == 3 &&
                                   g_clip_history_current == 2 &&
                                   clipboard_kind() == CLIP_IMAGE);
    T("Use an old one: it is on the clipboard again",
      clipboard_history_use(0) && clipboard_kind() == CLIP_TEXT &&
          !strcmp(clipboard_text(), "one") && g_clip_history_current == 0);
    T("... the picture is still kept",
      clipboard_history_image(2) && clipboard_history_image(2)->w == 40);
    clipboard_history_remove(1);
    T("remove one", clipboard_history_count() == 2 &&
                        !strcmp(clipboard_history_get_text(0), "one") &&
                        clipboard_history_kind(1) == CLIP_IMAGE);
    for (int i = 0; i < 25; i++) {
        char t[8];
        snprintf(t, sizeof t, "t%d", i);
        clipboard_set_text(t, (int)strlen(t));
    }
    T("20 at most: the oldest go",
      clipboard_history_count() == 20 &&
          !strcmp(clipboard_history_get_text(19), "t24") &&
          !strcmp(clipboard_history_get_text(0), "t5"));
    static uint8_t big[560 * 560];
    memset(big, 2, sizeof big);
    clipboard_set_image(560, 560, big, 560, (const uint8_t (*)[3])s_pal);
    clipboard_set_image(560, 560, big, 560, (const uint8_t (*)[3])s_pal);
    int pics = 0;
    for (int i = 0; i < clipboard_history_count(); i++)
        pics += clipboard_history_kind(i) == CLIP_IMAGE;
    snprintf(b, sizeof b, "    two big pictures: %d kept, %d items\n", pics,
             clipboard_history_count());
    wr(1, b);
    T("two big pictures: only the last kept (by size)",
      pics == 1 && clipboard_image() && clipboard_image()->w == 560);
    T("... the text stays (a picture goes first)",
      clipboard_history_count() == 19 && /* (two went: it was full) */
          !strcmp(clipboard_history_get_text(17), "t24"));
    T("too big: refused",
      !clipboard_set_image(600, 600, big, 0, (const uint8_t (*)[3])s_pal));

    /* ClipView: the list, the preview, Save, Use, Delete */
    clipboard_history_clear();
    clipboard_set_text("hello\nworld", (int)strlen("hello\nworld"));
    clipboard_set_image(40, 30, pix, 40, (const uint8_t (*)[3])s_pal);
    window *w = clipview();
    control *list = nth(w, CTL_LIST, 0), *text = nth(w, CTL_EDIT, 0);
    control *view = nth(w, CTL_CUSTOM, 0);
    T("ClipView: two items", w && list && list_count(list) == 2);
    snprintf(b, sizeof b, "    status: %s\n", status(w));
    wr(1, b);
    T("the picture first, on the clipboard",
      !strcmp(status(w), "A picture, 40 x 30: on the clipboard"));
    T("... shown as a picture", view && view->visible && !text->visible);
    int red = 0, blue = 0;
    for (int y = 0; view && y < view->ah; y++)
        for (int x = 0; x < view->aw; x++) {
            uint8_t v = BACKBUF[(view->ay + y) * SCREEN_WIDTH + view->ax + x];
            red += v == 4;
            blue += v == 1;
        }
    snprintf(b, sizeof b, "    preview: %d red, %d blue pixels\n", red, blue);
    wr(1, b);
    T("... both halves", red >= 500 && blue >= 500);
    sim_shot("scrapbook");
    press(w, "Save...");
    window *d = wm_focused_window();
    control *e = nth(d, CTL_EDIT, 0);
    if (e) {
        ctl_set_text(e, "CVPIC.BMP");
        ctl_focus(e);
    }
    sim_key(0x1C);
    sim_frames(3);
    T("Save...: a .BMP of it", bmp_is("/CVPIC.BMP", 40, 30));
    list_set_sel(list, 1);
    ctl_invoke(list, EV_CHANGE);
    sim_frames(1);
    T("the text: shown as text",
      text->visible && !view->visible && !strcmp(text->text, "hello\nworld"));
    press(w, "Use");
    T("Use: the text on the clipboard",
      clipboard_kind() == CLIP_TEXT &&
          !strcmp(clipboard_text(), "hello\nworld"));
    T("... marked", list_count(list) == 2 &&
                        !strncmp(status(w), "Text, 11 characters: on", 23));
    press(w, "Delete");
    T("Delete: one left",
      list_count(list) == 1 && clipboard_history_count() == 1);
    clipboard_set_text("new", (int)strlen("new"));
    sim_frames(2);
    T("something copied: at the top",
      list_count(list) == 2 && !strncmp(status(w), "Text, 3", 7));
    wm_close(w);
    sim_frames(2);

    /* pasted into a folder: a new .BMP; BMP Viewer copies one */
    fs_delete("/PICTURE.BMP");
    fs_delete("/PICTURE2.BMP");
    clipboard_set_image(40, 30, pix, 40, (const uint8_t (*)[3])s_pal);
    T("paste a picture into a folder: PICTURE.BMP",
      fo_can_paste() && OK(fo_paste("/")) && bmp_is("/PICTURE.BMP", 40, 30));
    T("... again: PICTURE2.BMP",
      OK(fo_paste("/")) && bmp_is("/PICTURE2.BMP", 40, 30));
    clipboard_set_text("x", (int)strlen("x"));
    T("text: nothing to paste in a folder", !fo_can_paste());
    appload_run_named("BMP Viewer", "/PICTURE.BMP");
    sim_frames(3);
    w = wm_focused_window();
    for (int i = 0; i < 600 && w && !strncmp(status(w), "Reading", 7); i++)
        sim_frames(1);
    run_menu(w, "Copy");
    const clip_image_t *im = clipboard_image();
    T("BMP Viewer, Edit > Copy: the picture",
      im && im->w == 40 && im->h == 30 && im->pix[0] == 4 && im->pix[39] == 1);
    wm_close(w);
    sim_frames(2);
    return fails;
}

int scen_bmp(const char *name) {
    if (!strcmp(name, "drawtools"))
        return draw_tools();
    if (!strcmp(name, "drawsel"))
        return draw_select();
    if (!strcmp(name, "scrapbook"))
        return scrapbook();
    if (!strcmp(name, "bmplib"))
        return kernel_lib();
    if (!strcmp(name, "bmpview"))
        return viewer();
    if (!strcmp(name, "drawbmp"))
        return draw_files();
    return -1;
}
