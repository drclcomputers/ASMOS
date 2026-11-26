// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* ClipView: the scrapbook. What was copied, text and pictures, newest
 * first (os/clipboard.h keeps the last 20); the one on the clipboard now
 * is marked. The preview shows the text, or the picture made to fit. Use
 * puts an item back on the clipboard; Save writes it as a .TXT or .BMP. */

#include "extapps/shim.h"

typedef struct {
    window *win; /* must stay first */
    control *list, *text, *view, *use, *del, *save, *clear, *status;
    int seen_count, seen_current, seen_kind; /* to see the scrapbook change */
    int item;                                /* the one shown, -1 none */
    uint8_t pal[256][3];                     /* the screen's */
    int ncol; /* the screen's colours to map to (16 or 248) */
    uint8_t lut[256];
    const clip_image_t *lut_for; /* the picture lut was made for */
    char label[24][64];          /* the list's lines */
} cv_state_t;

app_descriptor clipview_app;

static int item_at(cv_state_t *s) {
    int sel = list_sel(s->list);
    return sel < 0 ? -1 : (int)list_data(s->list, sel);
}

/* a picture's colours on this screen: the same index where the colour is
 * the same (the custom ones too), else the nearest fixed one */
static void make_lut(cv_state_t *s, const clip_image_t *im) {
    if (s->lut_for == im)
        return;
    s->lut_for = im;
    int keep = s->ncol > 16 ? 256 : 16;
    for (int i = 0; i < 256; i++) {
        if (i < keep && !memcmp(im->pal[i], s->pal[i], 3)) {
            s->lut[i] = (uint8_t)i;
            continue;
        }
        int best = 0, bd = 0x7FFFFFFF;
        for (int k = 0; k < s->ncol; k++) {
            int d = 0;
            for (int c = 0; c < 3; c++) {
                int e = im->pal[i][c] - s->pal[k][c];
                d += e * e * (c == 1 ? 4 : c == 0 ? 2 : 3);
            }
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        s->lut[i] = (uint8_t)best;
    }
}

/* the picture, as big as fits (never bigger than it is), in the middle */
static void paint_view(control *c, int x, int y, int w, int h) {
    cv_state_t *s = c->user;
    fill_rect(x, y, w, h, DARK_GRAY);
    const clip_image_t *im = clipboard_history_image(s->item);
    if (!im || w < 2 || h < 2)
        return;
    make_lut(s, im);
    int dw = im->w, dh = im->h; /* shrunk to fit, the same shape */
    if (dw > w - 2) {
        dh = dh * (w - 2) / dw;
        dw = w - 2;
    }
    if (dh > h - 2) {
        dw = dw * (h - 2) / dh;
        dh = h - 2;
    }
    dw = dw < 1 ? 1 : dw > 800 ? 800 : dw;
    dh = dh < 1 ? 1 : dh;
    static uint8_t row[800];
    int ox = x + (w - dw) / 2, oy = y + (h - dh) / 2;
    for (int r = 0; r < dh; r++) {
        const uint8_t *src = im->pix + (r * im->h / dh) * im->w;
        for (int k = 0; k < dw; k++)
            row[k] = s->lut[src[k * im->w / dw]];
        draw_pixels(ox, oy + r, dw, 1, row, dw);
    }
    draw_rect(ox - 1, oy - 1, dw + 2, dh + 2, BLACK);
}

/* the item picked: its text or picture, and what can be done with it */
static void show_item(cv_state_t *s) {
    s->item = item_at(s);
    int k = clipboard_history_kind(s->item);
    const char *t = clipboard_history_get_text(s->item);
    ctl_show(s->text, k != CLIP_IMAGE);
    ctl_show(s->view, k == CLIP_IMAGE);
    ctl_set_text(s->text, t ? t : "");
    ctl_enable(s->use, k != CLIP_EMPTY);
    ctl_enable(s->del, k != CLIP_EMPTY);
    ctl_enable(s->save, k != CLIP_EMPTY);
    ctl_enable(s->clear, clipboard_history_count() > 0);
    char b[80];
    const clip_image_t *im = clipboard_history_image(s->item);
    const char *on =
        s->item == g_clip_history_current ? ": on the clipboard" : "";
    if (k == CLIP_IMAGE)
        snprintf(b, sizeof(b), "A picture, %d x %d%s", im->w, im->h, on);
    else if (k == CLIP_TEXT)
        snprintf(b, sizeof(b), "Text, %d characters%s", (int)strlen(t), on);
    else
        snprintf(b, sizeof(b), "The scrapbook is empty: copy something.");
    ctl_set_text(s->status, b);
    wm_invalidate(s->win);
}

static void refill(cv_state_t *s) {
    int keep = item_at(s); /* (stay on it, if it is still there) */
    bool grew = clipboard_history_count() > s->seen_count;
    s->seen_count = clipboard_history_count();
    s->seen_current = g_clip_history_current;
    s->seen_kind = clipboard_kind();
    list_clear(s->list);
    int n = 0, sel = 0;
    for (int i = s->seen_count - 1; i >= 0 && n < 24; i--, n++) {
        char *line = s->label[n];
        const char *mark = i == g_clip_history_current ? "> " : "  ";
        const clip_image_t *im = clipboard_history_image(i);
        if (im) {
            snprintf(line, 64, "%sPicture %d x %d", mark, im->w, im->h);
        } else {
            const char *t = clipboard_history_get_text(i);
            int k = snprintf(line, 64, "%s", mark);
            for (int j = 0; t && t[j] && k < 60; j++)
                line[k++] =
                    (t[j] == '\n' || t[j] == '\r' || t[j] == '\t') ? ' ' : t[j];
            line[k] = 0;
        }
        list_add_data(s->list, line, (uint32_t)i);
        if (i == keep && !grew)
            sel = n;
    }
    list_set_sel(s->list, sel); /* (something new: it, at the top) */
    show_item(s);
}

static void on_list(control *c, int ev) {
    cv_state_t *s = c->user;
    if (ev == EV_CHANGE)
        show_item(s);
    if (ev == EV_SUBMIT && clipboard_history_use(item_at(s)))
        refill(s);
}

static void on_use(control *c, int ev) {
    cv_state_t *s = c->user;
    (void)ev;
    if (clipboard_history_use(item_at(s)))
        refill(s);
}

static void on_delete(control *c, int ev) {
    cv_state_t *s = c->user;
    (void)ev;
    int sel = list_sel(s->list);
    clipboard_history_remove(item_at(s));
    s->lut_for = NULL;
    refill(s);
    int n = list_count(s->list);
    list_set_sel(s->list, sel < n ? sel : n - 1);
    show_item(s);
}

static void cleared(void *user, bool yes) {
    cv_state_t *s = user;
    if (!yes)
        return;
    clipboard_history_clear();
    s->lut_for = NULL;
    refill(s);
}

static void on_clear(control *c, int ev) {
    cv_state_t *s = c->user;
    (void)ev;
    dlg_confirm(s->win, "ClipView", "Empty the scrapbook?", cleared, s);
}

/* Save: the text as it is, the picture as a .BMP (16 colours if it only
 * uses those) */
static void saved_to(void *user, const char *path) {
    cv_state_t *s = user;
    if (!path)
        return;
    const char *e = "Saved.";
    const clip_image_t *im = clipboard_history_image(s->item);
    const char *t = clipboard_history_get_text(s->item);
    if (im) {
        int top = 0;
        for (int i = 0; i < im->w * im->h; i++)
            top = im->pix[i] > top ? im->pix[i] : top;
        const char *err =
            bmp_save(path, im->pix, im->w, im->h, im->w,
                     (const uint8_t (*)[3])im->pal, top < 16 ? 16 : 256);
        e = err ? err : e;
    } else if (t) {
        e = A->file_write(path, t, (int)strlen(t)) ? e : "Save failed.";
    }
    ctl_set_text(s->status, e);
}

static void on_save(control *c, int ev) {
    cv_state_t *s = c->user;
    (void)ev;
    bool pic = clipboard_history_kind(s->item) == CLIP_IMAGE;
    dlg_save_file(s->win, pic ? "Save the picture" : "Save the text", "/",
                  pic ? "PICTURE" : "CLIP", pic ? "BMP" : "TXT", saved_to, s);
}

static void on_close(void) { os_quit_app_by_desc(&clipview_app); }
static void on_about(void) {
    dlg_message(NULL, "About ClipView",
                "ClipView v3.0: the scrapbook.\nThe last 20 things copied, "
                "text and pictures. Use (or double-click) puts one back on "
                "the clipboard; Save keeps it as a .TXT or .BMP file.");
}

static void cv_init(void *state) {
    cv_state_t *s = state;
    s->win =
        app_window(s, "ClipView", 30, 30, 260, 190, DARK_GRAY, AW_RESIZABLE);
    if (!s->win)
        return;
    s->win->min_w = 230;
    s->win->min_h = 150;
    int cx, cy, cw, ch;
    win_client_rect(s->win, &cx, &cy, &cw, &ch);
    int by = ch - 11 - 15; /* the buttons' row, over the status line */
    s->list = ctl_list(s->win, 4, 4, cw - 8, 50, CS_STRETCH_W, on_list);
    s->text = ctl_edit(s->win, 4, 58, cw - 8, by - 62, "", 0,
                       ES_MULTILINE | ES_READONLY | CS_STRETCH_W | CS_STRETCH_H,
                       NULL);
    s->view = ctl_custom(s->win, 4, 58, cw - 8, by - 62, paint_view, NULL,
                         CS_BORDER | CS_STRETCH_W | CS_STRETCH_H);
    static const char *const LABEL[4] = {"Use", "Delete", "Save...",
                                         "Clear all"};
    static const char *const TIP[4] = {
        "Put it back on the clipboard (or double-click it)",
        "Take it out of the scrapbook", "Keep it in a .TXT or .BMP file",
        "Empty the scrapbook"};
    control **b[4] = {&s->use, &s->del, &s->save, &s->clear};
    void (*fn[4])(control *, int) = {on_use, on_delete, on_save, on_clear};
    for (int i = 0; i < 4; i++) {
        *b[i] = ctl_button(s->win, 4 + i * 54, by, 50, 13, LABEL[i], 0, fn[i]);
        ctl_style_add(*b[i], CS_ANCHOR_BOTTOM);
        ctl_set_tooltip(*b[i], TIP[i]);
    }
    s->status = ctl_status(s->win, "");
    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "Close", on_close);
    menu_add_separator(f);
    menu_add_item(f, "About", on_about);
    int n = A->screen_palette(s->pal, 256);
    s->ncol = n > APP_COLOR_CUSTOM ? APP_COLOR_CUSTOM : n;
    s->item = -1;
    refill(s);
    ctl_focus(s->list);
}

static void cv_on_frame(void *state) {
    cv_state_t *s = state;
    if (clipboard_history_count() != s->seen_count ||
        g_clip_history_current != s->seen_current ||
        clipboard_kind() != s->seen_kind)
        refill(s);
}

APP_META("ClipView", cv_state_t, true, 0, cv_init, cv_on_frame, NULL);
