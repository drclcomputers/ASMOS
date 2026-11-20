/* Character Map - every character of the code page (Settings > Display), in
 * any of the fonts: pick one, see it large with its codes (and the
 * Alt + number pad keys that type it), gather a few, copy them. */
#include "fonts/fonts.h"
#include "os/api.h"
#include "os/clipboard.h"
#include "ui/uifont.h"

#define COLS 16
#define FIRST 32 /* the grid: 32..255 */
#define ROWS ((256 - FIRST) / COLS)
#define CW 14 /* a cell */
#define CH 9
#define GX 4 /* the grid's place */
#define GY 20
#define GW (COLS * CW + 4) /* with its border (2 a side) */
#define GH (ROWS * CH + 4)
#define PX (GX + GW + 3) /* the large one, its codes */

typedef struct {
    window *win; /* must stay first */
    control *font, *cpl, *grid, *big, *text, *copy;
    int sel; /* the character picked */
    int shown_cp;
} cm_state_t;

app_descriptor charmap_app;

static const int FONTS[] = {FONT_SYSTEM, FONT_SMALL, FONT_MONO8, FONT_MONO};

static int cur_font(cm_state_t *s) {
    int i = ctl_value(s->font);
    return FONTS[i >= 0 && i < 4 ? i : 1];
}

static bool usable(int c) { return c != 127 && font_has_char((uint8_t)c); }

/* ── the grid ─────────────────────────────────────────────────────────── */

static void paint_grid(control *c, int x, int y, int w, int h) {
    cm_state_t *s = c->user;
    fill_rect(x, y, w, h, WHITE);
    int f = cur_font(s), fh = font_height(f);
    for (int i = 0; i < ROWS * COLS; i++) {
        int ch = FIRST + i, cx = x + (i % COLS) * CW, cy = y + (i / COLS) * CH;
        bool on = ch == s->sel;
        if (on)
            fill_rect(cx + 1, cy + 1, CW - 1, CH - 1, BLUE);
        if (!usable(ch)) {
            fill_rect(cx + 5, cy + 5, 4, 1, LIGHT_GRAY); /* nothing there */
            continue;
        }
        char t[2] = {(char)ch, 0};
        /* (row 0 of a cell is the line above it) */
        draw_string(cx + (CW - text_width(t, f) + 1) / 2,
                    cy + 1 + (CH - 1 - fh) / 2, t, on ? WHITE : BLACK, f);
    }
    for (int k = 1; k < COLS; k++) /* the lines between */
        fill_rect(x + k * CW, y, 1, h, LIGHT_GRAY);
    for (int r = 1; r < ROWS; r++)
        fill_rect(x, y + r * CH, w, 1, LIGHT_GRAY);
}

static void pick(cm_state_t *s, int ch) {
    if (ch < FIRST || ch > 255)
        return;
    s->sel = ch;
    wm_invalidate(s->win);
}

static void add_sel(cm_state_t *s) {
    if (!usable(s->sel))
        return;
    char t[2] = {(char)s->sel, 0};
    const char *old = ctl_text(s->text);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s", old, t);
    ctl_set_text(s->text, buf);
}

static void on_grid(control *c, int ev) {
    cm_state_t *s = c->user;
    if (ev == EV_MOUSEDOWN || ev == EV_DBLCLICK) {
        int col = (c->mx - 2) / CW, row = (c->my - 2) / CH;
        if (col >= 0 && col < COLS && row >= 0 && row < ROWS)
            pick(s, FIRST + row * COLS + col);
        if (ev == EV_DBLCLICK)
            add_sel(s);
        return;
    }
    if (ev != EV_KEY)
        return;
    switch (kb.last_scancode) {
    case LEFT_ARROW:
        pick(s, s->sel - 1);
        break;
    case RIGHT_ARROW:
        pick(s, s->sel + 1);
        break;
    case UP_ARROW:
        pick(s, s->sel - COLS);
        break;
    case DOWN_ARROW:
        pick(s, s->sel + COLS);
        break;
    case ENTER:
    case SPACE:
        add_sel(s);
        break;
    }
}

/* ── the large one ────────────────────────────────────────────────────── */

static void paint_big(control *c, int x, int y, int w, int h) {
    cm_state_t *s = c->user;
    fill_rect(x, y, w, h, WHITE);
    if (!usable(s->sel))
        return;
    uint8_t rows[8];
    int gw = 0, f = cur_font(s);
    int gh = font_glyph(f, (uint8_t)s->sel, rows, &gw);
    int z = 4;
    while (z > 1 && (gw * z > w - 2 || gh * z > h - 2))
        z--;
    int ox = x + (w - gw * z) / 2, oy = y + (h - gh * z) / 2;
    for (int r = 0; r < gh; r++)
        for (int k = 0; k < 8; k++)
            if (rows[r] & (0x80 >> k))
                fill_rect(ox + k * z, oy + r * z, z, z, BLACK);
}

static void paint_codes(control *c, int x, int y, int w, int h) {
    cm_state_t *s = c->user;
    (void)w, (void)h;
    int f = UI_FONT_SMALL, lh = font_height(f) + 2;
    char t[5][16];
    int ch = s->sel;
    unsigned u = ch < 128 ? (unsigned)ch : cp_unicode[g_codepage][ch - 128];
    snprintf(t[0], 16, "%d", ch);
    snprintf(t[1], 16, "%02Xh", ch);
    snprintf(t[2], 16, "Alt+");
    snprintf(t[3], 16, "%s%d", ch >= 128 ? "0" : "", ch);
    snprintf(t[4], 16, u ? "U+%04X" : "-", u);
    for (int i = 0; i < 5; i++)
        draw_string(x, y + i * lh, t[i], i == 2 || i == 3 ? DARK_GRAY : BLACK,
                    f);
}

/* ── the rest ─────────────────────────────────────────────────────────── */

static void show_cp(cm_state_t *s) {
    s->shown_cp = g_codepage;
    ctl_set_text(s->cpl, cp_name[g_codepage]);
    wm_invalidate(s->win);
}

static void on_font(control *c, int ev) {
    if (ev == EV_CHANGE)
        wm_invalidate(((cm_state_t *)c->user)->win);
}

static void on_copy(control *c, int ev) {
    cm_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    const char *t = ctl_text(s->text);
    char one[2] = {(char)s->sel, 0};
    if (!t[0]) /* nothing gathered: the one picked */
        t = usable(s->sel) ? one : "";
    if (t[0])
        clipboard_set_text(t, (int)strlen(t));
}

static void m_close(void) { os_quit_app_by_desc(&charmap_app); }
static void m_about(void) {
    dlg_message(NULL, "About Character Map",
                "Character Map 1.0\nThe characters of the code page in "
                "use\n(Settings > Display > Characters).\nDouble-click or "
                "Enter: add it to the box.\nType one anywhere: Alt + its "
                "code\non the number pad (Alt+0233).");
}

static void cm_init(void *state) {
    cm_state_t *s = state;
    s->win = app_window(s, "Character Map", 20, 11, PX + 44, 184, DARK_GRAY, 0);
    if (!s->win)
        return;
    window *w = s->win;
    ctl_label(w, 4, 5, 26, 12, "Font:", 0);
    s->font = ctl_combo(w, 32, 4, 78, on_font);
    list_add(s->font, "System");
    list_add(s->font, "Small");
    list_add(s->font, "Fixed 8x8");
    list_add(s->font, "Fixed 4x6");
    list_set_sel(s->font, 1);
    s->cpl = ctl_label(w, 114, 5, PX + 36 - 114, 12, "", LS_RIGHT);
    ctl_set_tooltip(s->cpl, "Settings > Display > Characters changes it");
    s->grid = ctl_custom(w, GX, GY, GW, GH, paint_grid, on_grid,
                         CS_BORDER | CS_FOCUSABLE);
    s->big = ctl_custom(w, PX, GY, 36, 36, paint_big, NULL, CS_BORDER);
    ctl_custom(w, PX, GY + 40, 36, 50, paint_codes, NULL, 0);
    s->text = ctl_edit(w, GX, GY + GH + 4, GW - 46, 12, "", 60, 0, NULL);
    ctl_set_tooltip(s->text, "Double-click characters to gather them here");
    s->copy =
        ctl_button(w, GX + GW - 42, GY + GH + 4, 42, 13, "Copy", 1, on_copy);
    ctl_set_tooltip(s->copy, "To the clipboard (the box, or the one picked)");
    menu *m = window_add_menu(w, "File");
    menu_add_item(m, "Close", m_close);
    menu_add_separator(m);
    menu_add_item(m, "About Character Map", m_about);
    s->sel = 0xE9; /* e-acute in 1252 and 1250 (theta in 437) */
    show_cp(s);
    ctl_focus(s->grid);
}

static void cm_frame(void *state) {
    cm_state_t *s = state;
    if (s->win && s->shown_cp != g_codepage) /* Settings changed it */
        show_cp(s);
}

app_descriptor charmap_app = {
    .name = "CHARMAP",
    .state_size = sizeof(cm_state_t),
    .init = cm_init,
    .on_frame = cm_frame,
    .single_instance = true,
};
