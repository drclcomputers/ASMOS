// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* Solitaire (extapps/solitaire.c): the deals, the rules through clicks and
 * drags, Spider's suits leaving, FreeCell's numbered deals, the game kept */
#include "scen.h"

/* the app's state, as far as the tests look (extapps/solitaire.c) */
#define MAXC 104
#define NPILES 24
typedef struct {
    uint8_t from, to, n, flags;
} step_t;
typedef struct {
    uint8_t kind, n;
    uint8_t c[MAXC];
    int16_t x, y;
    uint8_t fd, fu;
} pile_t;
typedef struct {
    window *win;
    const void *api;
    control *view, *status;
    int mode;
    uint32_t game;
    pile_t p[NPILES];
    int np;
    int stock, waste, found, nfound, cell, ncell, tab, ntab;
    step_t undo[1200];
    int nundo;
    int moves;
    bool won, started;
    uint32_t start_ms, elapsed_ms;
    int shown_secs;
    int cw, ch, gap, lay_w, lay_h;
} sol_t;

enum { KLONDIKE1, KLONDIKE3, SPIDER1, SPIDER2, SPIDER4, FREECELL };
#define FACE 0x80
#define CARD(r, su) ((uint8_t)((r) | (su) << 4))
enum { CLUBS, DIAMONDS, HEARTS, SPADES };

static window *s_w;
static sol_t *S;

static bool menu_run(const char *label) {
    for (int m = 0; m < s_w->win_menu_count; m++)
        for (int k = 0; k < s_w->win_menus[m].item_count; k++) {
            menu_item *it = &s_w->win_menus[m].items[k];
            if (it->label && !strcmp(it->label, label) && it->action_ud) {
                g_menu_call_hook(it->dyn_state, it->action_ud, it->userdata);
                return true;
            }
            menu *sub = it->sub;
            for (int j = 0; sub && j < sub->item_count; j++)
                if (sub->items[j].label &&
                    !strcmp(sub->items[j].label, label) &&
                    sub->items[j].action_ud) {
                    g_menu_call_hook(sub->items[j].dyn_state,
                                     sub->items[j].action_ud,
                                     sub->items[j].userdata);
                    return true;
                }
        }
    return false;
}

/* where card k of pile p is (its middle-top, a spot that shows) */
static void spot(int pi, int k, int *x, int *y) {
    pile_t *p = &S->p[pi];
    int yy = p->y;
    for (int j = 0; j < k && p->kind == 4; j++)
        yy += p->c[j] & FACE ? p->fu : p->fd;
    *x = p->x + S->cw / 2;
    *y = yy + 3;
}

/* with the real mouse: the screen is the view's place plus x, y */
static void click(int pi, int k) {
    int x, y;
    sim_frames(1); /* (the layout of what was set up) */
    spot(pi, k, &x, &y);
    sim_click(S->view->ax + x, S->view->ay + y);
    sim_frames(1);
}

static void drag(int pi, int k, int to) {
    int x, y, tx, ty;
    sim_frames(1);
    spot(pi, k, &x, &y);
    pile_t *q = &S->p[to];
    spot(to, q->n ? q->n - 1 : 0, &tx, &ty);
    int ax = S->view->ax, ay = S->view->ay;
    sim_drag(ax + x, ay + y, ax + tx, ay + ty + 6);
    sim_frames(1);
}

static void set_pile(int pi, const uint8_t *c, int n) {
    S->p[pi].n = (uint8_t)n;
    for (int k = 0; k < n; k++)
        S->p[pi].c[k] = c[k];
}

static void clear_all(void) {
    for (int i = 0; i < S->np; i++)
        S->p[i].n = 0;
    S->nundo = 0;
}

static int count(void) {
    int n = 0;
    for (int i = 0; i < S->np; i++)
        n += S->p[i].n;
    return n;
}

/* every card of the deal once (twice: Spider's two decks) */
static bool all_there(int per) {
    uint8_t seen[64] = {0};
    for (int i = 0; i < S->np; i++)
        for (int k = 0; k < S->p[i].n; k++)
            seen[S->p[i].c[k] & 63]++;
    int n = 0;
    for (int k = 0; k < 64; k++) {
        if (seen[k] && seen[k] != per)
            return false;
        n += seen[k];
    }
    return n == count();
}

static bool launch(void) {
    static uint8_t buf[65536];
    uint32_t n = 0;
    if (!load_file(ROOT "build/extapps/solitaire.APP", buf, sizeof buf, &n))
        return false;
    fat_file_t f;
    fs_delete("/SOLITAIR.APP");
    fs_create("/SOLITAIR.APP", &f);
    fs_write(&f, buf, n);
    fs_close(&f);
    if (!appload_launch("/SOLITAIR.APP")) {
        wr(2, appload_last_error());
        wr(2, "\n");
        return false;
    }
    sim_frames(5);
    s_w = wm_focused_window();
    S = s_w ? s_w->dyn_state : NULL;
    return S != NULL;
}

static const char *name(uint8_t c) {
    static char b[4];
    const char *r = "?A23456789TJQK";
    b[0] = r[c & 15];
    b[1] = "CDHS"[(c >> 4) & 3];
    b[2] = 0;
    return b;
}

static int sol_test(void) {
    int fails = 0;
    char b[300];
    fs_delete("/PREFS/SOLITAIR.SAV");
    fs_delete("/PREFS/SOLITAIR.INI");
    if (!launch()) {
        T("launched", false);
        return fails;
    }
    T("a window, lazy", s_w->lazy_custom && !strcmp(s_w->title, "Solitaire"));

    /* Klondike */
    T("starts with Klondike", S->mode == KLONDIKE1);
    bool cols = true;
    for (int k = 0; k < 7; k++) {
        pile_t *p = &S->p[S->tab + k];
        cols = cols && p->n == k + 1 && (p->c[k] & FACE);
        for (int j = 0; j < k; j++)
            cols = cols && !(p->c[j] & FACE);
    }
    T("columns of 1 to 7, the top card up", cols);
    T("24 in the stock, all 52 there",
      S->p[S->stock].n == 24 && count() == 52 && all_there(1));
    sim_shot("sol_k");
    click(S->stock, 0);
    T("the stock clicked: a card turned up on the waste",
      S->p[S->waste].n == 1 && (S->p[S->waste].c[0] & FACE) && S->moves == 1);
    menu_run("Undo  Ctrl+Z");
    T("undone", S->p[S->waste].n == 0 && S->p[S->stock].n == 24 &&
                    !(S->p[S->stock].c[23] & FACE));

    /* the rules, on a set-up table */
    clear_all();
    uint8_t a[] = {CARD(7, HEARTS) | FACE}, c6[] = {CARD(6, SPADES) | FACE},
            c6r[] = {CARD(6, DIAMONDS) | FACE};
    set_pile(S->tab, a, 1);
    set_pile(S->tab + 1, c6, 1);
    set_pile(S->tab + 2, c6r, 1);
    sim_frames(1);
    click(S->tab + 2, 0);
    T("a red 6 doesn't go on a red 7", S->p[S->tab + 2].n == 1);
    click(S->tab + 1, 0);
    T("a click: the black 6 onto the red 7",
      S->p[S->tab].n == 2 && S->p[S->tab + 1].n == 0);
    T("an empty column takes only a King", S->p[S->tab + 1].n == 0);
    menu_run("Undo  Ctrl+Z");
    T("undone", S->p[S->tab].n == 1 && S->p[S->tab + 1].n == 1);
    drag(S->tab + 1, 0, S->tab);
    T("dragged there too", S->p[S->tab].n == 2);
    uint8_t ace[] = {CARD(1, CLUBS) | FACE};
    set_pile(S->tab + 3, ace, 1);
    click(S->tab + 3, 0);
    T("an Ace clicked goes up", S->p[S->found].n == 1);
    /* the last card: won */
    clear_all();
    for (int su = 0; su < 4; su++)
        for (int r = 1; r <= (su == 3 ? 12 : 13); r++)
            S->p[S->found + su].c[S->p[S->found + su].n++] = CARD(r, su) | FACE;
    uint8_t k[] = {CARD(13, SPADES) | FACE};
    set_pile(S->tab + 6, k, 1);
    sim_frames(1);
    click(S->tab + 6, 0);
    T("the last King up: won", S->won);
    sim_frames(3);
    sim_shot("sol_won");
    sim_key(0x1C);
    sim_frames(3);
    T("counted: a game won", app_prefs_get_int("Solitaire", "w0", 0) == 1);

    /* Klondike, draw 3: finishing by itself */
    menu_run("Klondike (Draw 3)");
    sim_frames(2);
    T("Draw 3", S->mode == KLONDIKE3 && S->p[S->stock].n == 24);
    click(S->stock, 0);
    T("three turned", S->p[S->waste].n == 3);
    clear_all();
    for (int su = 0; su < 4; su++)
        for (int r = 1; r <= 10; r++)
            S->p[S->found + su].c[S->p[S->found + su].n++] = CARD(r, su) | FACE;
    for (int su = 0; su < 4; su++) {
        uint8_t col[] = {CARD(13, su) | FACE, CARD(12, (su + 1) % 4) | FACE,
                         CARD(11, su) | FACE};
        set_pile(S->tab + su, col, 3);
    }
    /* (J of suit 3 must go first for the move to start) */
    uint8_t col4[] = {CARD(13, 3) | FACE, CARD(12, 0) | FACE};
    set_pile(S->tab + 3, col4, 2);
    uint8_t col5[] = {CARD(11, 3) | FACE};
    set_pile(S->tab + 5, col5, 1);
    sim_frames(1);
    click(S->tab + 5, 0);
    T("all turned up, no stock: the rest goes up by itself", S->won);
    sim_key(0x1C);
    sim_frames(3);

    /* FreeCell: Microsoft's deal #1 */
    menu_run("FreeCell");
    sim_frames(2);
    S->moves = 0;
    void (*picked)(void *, const char *) = NULL;
    (void)picked;
    menu_run("Select Game...");
    sim_frames(2);
    window *d = wm_focused_window();
    for (control *c = d->controls; c; c = c->next)
        if (c->type == CTL_EDIT)
            ctl_set_text(c, "1");
    sim_key(0x1C);
    sim_frames(3);
    char first[40] = "";
    for (int k = 0; k < 8; k++) {
        strcat(first, name(S->p[S->tab + k].c[0]));
        strcat(first, k < 7 ? " " : "");
    }
    snprintf(b, sizeof b, "  (game #%u: %s)\n", S->game, first);
    wr(1, b);
    T("FreeCell #1 is the classic one",
      S->game == 1 && !strcmp(first, "JD 2D 9H JC 5D 7H 7C 5H"));
    T("columns of 7 and 6, all face up, 52 there",
      S->p[S->tab].n == 7 && S->p[S->tab + 7].n == 6 && all_there(1));
    sim_shot("sol_fc");
    /* moving several at once: through the cells */
    clear_all();
    uint8_t run[] = {CARD(13, SPADES) | FACE, CARD(9, CLUBS) | FACE,
                     CARD(8, HEARTS) | FACE,  CARD(7, CLUBS) | FACE,
                     CARD(6, HEARTS) | FACE,  CARD(5, CLUBS) | FACE};
    uint8_t ten[] = {CARD(10, HEARTS) | FACE};
    uint8_t filler[] = {CARD(2, CLUBS) | FACE};
    set_pile(S->tab, run, 6);
    set_pile(S->tab + 1, ten, 1);
    for (int k = 2; k < 8; k++)
        set_pile(S->tab + k, filler, 1);
    uint8_t q[] = {CARD(12, SPADES) | FACE}, j[] = {CARD(12, CLUBS) | FACE},
            x[] = {CARD(12, HEARTS) | FACE};
    set_pile(S->cell, q, 1);
    set_pile(S->cell + 1, j, 1);
    set_pile(S->cell + 2, x, 1);
    sim_frames(1);
    drag(S->tab, 1, S->tab + 1);
    T("one free cell: two at a time, not five", S->p[S->tab + 1].n == 1);
    S->p[S->cell + 2].n = 0;
    S->p[S->cell + 1].n = 0;
    drag(S->tab, 1, S->tab + 1);
    T("three free cells: four at a time, not five", S->p[S->tab + 1].n == 1);
    S->p[S->cell].n = 0;
    drag(S->tab, 1, S->tab + 1);
    T("four free cells: five move", S->p[S->tab + 1].n == 6);
    menu_run("Undo  Ctrl+Z");
    click(S->tab, 5);
    T("a click: the 5 to a free cell if nothing else",
      S->p[S->tab].n == 5 && (S->p[S->cell].n + S->p[S->cell + 1].n +
                              S->p[S->cell + 2].n + S->p[S->cell + 3].n) == 1);

    /* Spider */
    menu_run("Spider (1 Suit)");
    sim_frames(2);
    T("Spider: 10 columns, 54 out, 50 in the stock",
      S->ntab == 10 && S->p[S->tab].n == 6 && S->p[S->tab + 9].n == 5 &&
          S->p[S->stock].n == 50 && count() == 104);
    sim_shot("sol_sp");
    click(S->stock, 0);
    T("a deal: a card more on every column, face up",
      S->p[S->stock].n == 40 && S->p[S->tab + 9].n == 6 &&
          (S->p[S->tab + 9].c[5] & FACE));
    menu_run("Undo  Ctrl+Z");
    T("undone: back in the stock",
      S->p[S->stock].n == 50 && S->p[S->tab + 9].n == 5);
    /* a whole suit leaves */
    clear_all();
    uint8_t suit[13];
    for (int r = 13; r >= 2; r--)
        suit[13 - r] = CARD(r, SPADES) | FACE;
    uint8_t under[] = {CARD(4, SPADES), CARD(9, SPADES) | FACE};
    set_pile(S->tab, under, 2);
    for (int i = 0; i < 12; i++)
        S->p[S->tab].c[2 + i] = suit[i];
    S->p[S->tab].n = 14;
    uint8_t acec[] = {CARD(1, SPADES) | FACE};
    set_pile(S->tab + 1, acec, 1);
    for (int k = 2; k < 10; k++)
        set_pile(S->tab + k, under + 1, 1);
    sim_frames(1);
    click(S->tab + 1, 0);
    T("King to Ace in one suit: it leaves, the card under turns up",
      S->p[S->found].n == 13 && S->p[S->tab].n == 2 &&
          (S->p[S->tab].c[1] & FACE) && !(S->p[S->tab].c[0] & FACE));
    sim_shot("sol_sp2");
    menu_run("Undo  Ctrl+Z");
    T("one undo puts it all back",
      S->p[S->found].n == 0 && S->p[S->tab].n == 14 && S->p[S->tab + 1].n == 1);
    /* only one suit moves together */
    menu_run("Spider (2 Suits)");
    sim_frames(2);
    T("two suits: spades and hearts, 104", all_there(4) && count() == 104);
    clear_all();
    uint8_t mixed[] = {CARD(8, SPADES) | FACE, CARD(7, HEARTS) | FACE};
    uint8_t nine[] = {CARD(9, HEARTS) | FACE};
    set_pile(S->tab, mixed, 2);
    set_pile(S->tab + 1, nine, 1);
    for (int k = 2; k < 10; k++)
        set_pile(S->tab + k, nine, 1);
    S->p[S->stock].n = 10;
    sim_frames(1);
    drag(S->tab, 0, S->tab + 1);
    T("8S 7H don't move together", S->p[S->tab + 1].n == 1);
    S->p[S->tab + 5].n = 0;
    click(S->stock, 0);
    sim_frames(2);
    T("an empty column: no deal", S->p[S->stock].n == 10);
    sim_key(0x1C);
    sim_frames(2);

    /* the game kept */
    menu_run("Klondike (Draw 1)");
    sim_frames(2);
    click(S->stock, 0);
    uint32_t g = S->game;
    uint8_t w0 = S->p[S->waste].c[0];
    wm_close(s_w);
    sim_frames(5);
    launch();
    T("closed and opened: the same game, where it was",
      S->game == g && S->mode == KLONDIKE1 && S->p[S->waste].n == 1 &&
          S->p[S->waste].c[0] == w0 && S->moves == 1);
    menu_run("Statistics...");
    sim_frames(3);
    sim_shot("sol_stats");
    sim_key(0x1C);
    sim_frames(2);
    wm_close(s_w);
    sim_frames(3);
    return fails;
}

/* each game at 320x200 */
static int sol_small(void) {
    int fails = 0;
    char b[300];
    extern int g_screen_width, g_screen_height, g_video_mode;
    g_screen_width = 320;
    g_screen_height = 200;
    g_video_mode = 0;
    fs_delete("/PREFS/SOLITAIR.SAV");
    if (!launch()) {
        T("launched", false);
        return fails;
    }
    T("fits the screen",
      s_w->x >= 0 && s_w->x + s_w->w <= 320 && s_w->y + s_w->h <= 200);
    sim_shot("sol320k");
    menu_run("Spider (4 Suits)");
    sim_frames(2);
    for (int i = 0; i < 4; i++)
        click(S->stock, 0);
    sim_frames(2);
    sim_shot("sol320s");
    int tallest = 0;
    for (int k = 0; k < S->ntab; k++) {
        pile_t *p = &S->p[S->tab + k];
        int y = p->y;
        for (int j = 0; j + 1 < p->n; j++)
            y += p->c[j] & FACE ? p->fu : p->fd;
        if (y + S->ch > tallest)
            tallest = y + S->ch;
    }
    T("long columns squeeze in", tallest <= S->view->ah);
    menu_run("FreeCell");
    sim_frames(2);
    sim_shot("sol320f");
    menu_run("Klondike (Draw 3)");
    sim_frames(2);
    click(S->stock, 0);
    int x, y;
    spot(S->tab + 6, 6, &x, &y);
    x += S->view->ax, y += S->view->ay;
    sim_move(x, y); /* (held: a shot mid-drag) */
    mouse.left = true;
    sim_frame();
    sim_move(x - 30, y + 12);
    sim_move(x - 40, y + 15);
    sim_frames(1);
    sim_shot("sol320d");
    mouse.left = false;
    sim_frames(2);
    wm_close(s_w);
    sim_frames(3);
    fs_delete("/PREFS/SOLITAIR.SAV");
    return fails;
}

int scen_sol(const char *name) {
    if (!strcmp(name, "solitaire"))
        return sol_test();
    if (!strcmp(name, "solitaire320"))
        return sol_small();
    return -1;
}
