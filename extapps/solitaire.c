// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "extapps/shim.h"

#define STATUS_H 12
#define MAXC 104 /* cards in a pile, at most */
#define NPILES 24
#define UNDO_MAX 1200
#define SAVE_PATH "/PREFS/SOLITAIR.SAV"
#define SAVE_MAGIC 0x534F4C31u /* "SOL1" */

/* draw_string sizes */
#define F_SMALL 2
#define F_BOLD 3

/* a card: rank 1..13 in the low 4 bits, the suit above, face up on top */
#define RANK(c) ((c) & 15)
#define SUIT(c) (((c) >> 4) & 3)
#define FACE 0x80
#define RED_SUIT(c) (SUIT(c) == 1 || SUIT(c) == 2) /* diamonds, hearts */
#define CARD(r, su) ((uint8_t)((r) | (su) << 4))

enum { CLUBS, DIAMONDS, HEARTS, SPADES };
enum { K_STOCK, K_WASTE, K_FOUND, K_CELL, K_TAB };
enum { KLONDIKE1, KLONDIKE3, SPIDER1, SPIDER2, SPIDER4, FREECELL, MODES };
static const char *const MODE_NAME[MODES] = {
    "Klondike (Draw 1)", "Klondike (Draw 3)", "Spider (1 Suit)",
    "Spider (2 Suits)",  "Spider (4 Suits)",  "FreeCell"};
static const char *const MODE_SHORT[MODES] = {
    "Klondike", "Klondike 3", "Spider 1", "Spider 2", "Spider 4", "FreeCell"};

/* an undo step: n cards from one pile to another */
#define U_FLIP 1    /* each card turned over (stock <-> waste, deals) */
#define U_EXPOSED 2 /* the card left on top of `from` was turned up */
#define U_CHAIN 4   /* undone together with the step before it */
typedef struct {
    uint8_t from, to, n, flags;
} step_t;

typedef struct {
    uint8_t kind, n;
    uint8_t c[MAXC];
    /* where (control coordinates), and the fan of a column */
    int16_t x, y;
    uint8_t fd, fu; /* rows per face-down / face-up card */
} pile_t;

typedef struct {
    window *win; /* must stay first (--window-first) */
    const os_api_t *api;
    control *view, *status;
    int mode;
    uint32_t game; /* the deal's number */
    pile_t p[NPILES];
    int np;
    int stock, waste, found, nfound, cell, ncell, tab, ntab; /* first, count */
    step_t undo[UNDO_MAX];
    int nundo;
    int moves;
    bool won, started;
    uint32_t start_ms, elapsed_ms;
    int shown_secs;
    /* card size (from the window size) */
    int cw, ch, gap, lay_w, lay_h;
    /* a press, and a drag */
    int press_pile, press_idx, grab_dx, grab_dy, down_x, down_y, mx, my;
    bool dragging;
    /* a hint: shown until hint_until */
    int hint_from, hint_idx, hint_to;
    uint32_t hint_until;
    uint32_t rng;
} sol_t;

// randomness: xorshift32 from the deal's number; FreeCell: Microsoft's

static uint32_t rnd(sol_t *s) {
    uint32_t x = s->rng ? s->rng : 0x2545F491u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s->rng = x;
}

static bool is_spider(const sol_t *s) {
    return s->mode >= SPIDER1 && s->mode <= SPIDER4;
}
static bool is_klondike(const sol_t *s) { return s->mode <= KLONDIKE3; }

// piles

static int add_pile(sol_t *s, int kind) {
    pile_t *p = &s->p[s->np];
    p->kind = (uint8_t)kind;
    p->n = 0;
    return s->np++;
}

static uint8_t top(const pile_t *p) { return p->n ? p->c[p->n - 1] : 0; }

/* n cards off the top of `from` onto `to` (in order), as one undo step */
static void move(sol_t *s, int from, int to, int n, int flags) {
    pile_t *a = &s->p[from], *b = &s->p[to];
    for (int k = 0; k < n; k++) {
        uint8_t c = a->c[a->n - n + k];
        b->c[b->n + k] = flags & U_FLIP ? (uint8_t)(c ^ FACE) : c;
    }
    a->n = (uint8_t)(a->n - n);
    b->n = (uint8_t)(b->n + n);
    if (a->kind == K_TAB && a->n && !(a->c[a->n - 1] & FACE)) {
        a->c[a->n - 1] |= FACE;
        flags |= U_EXPOSED;
    }
    if (s->nundo == UNDO_MAX) { /* (the oldest goes) */
        for (int k = 1; k < UNDO_MAX; k++)
            s->undo[k - 1] = s->undo[k];
        s->nundo--;
    }
    s->undo[s->nundo++] =
        (step_t){(uint8_t)from, (uint8_t)to, (uint8_t)n, (uint8_t)flags};
}

static void unmove(sol_t *s, const step_t *u) {
    pile_t *a = &s->p[u->from], *b = &s->p[u->to];
    if (u->flags & U_EXPOSED)
        a->c[a->n - 1] &= (uint8_t)~FACE;
    for (int k = 0; k < u->n; k++) {
        uint8_t c = b->c[b->n - u->n + k];
        a->c[a->n + k] = u->flags & U_FLIP ? (uint8_t)(c ^ FACE) : c;
    }
    a->n = (uint8_t)(a->n + u->n);
    b->n = (uint8_t)(b->n - u->n);
}

// the rules

/* cards from i to the top move together: a run */
static bool is_run(const sol_t *s, const pile_t *p, int i) {
    if (!(p->c[i] & FACE))
        return false;
    for (int k = i + 1; k < p->n; k++) {
        uint8_t a = p->c[k - 1], b = p->c[k];
        if (RANK(a) != RANK(b) + 1)
            return false;
        if (is_spider(s) ? SUIT(a) != SUIT(b) : RED_SUIT(a) == RED_SUIT(b))
            return false;
    }
    return true;
}

static bool can_pick(const sol_t *s, int pi, int i) {
    const pile_t *p = &s->p[pi];
    if (i < 0 || i >= p->n)
        return false;
    switch (p->kind) {
    case K_TAB:
        return is_run(s, p, i);
    case K_WASTE:
    case K_CELL:
        return i == p->n - 1;
    case K_FOUND:
        return i == p->n - 1 && is_klondike(s);
    default:
        return false;
    }
}

/* FreeCell: how many cards move at once (one at a time, through the free
 * cells and empty columns) */
static int capacity(const sol_t *s, int to) {
    int cells = 0, empty = 0;
    for (int k = 0; k < s->ncell; k++)
        cells += s->p[s->cell + k].n == 0;
    for (int k = 0; k < s->ntab; k++)
        empty += s->p[s->tab + k].n == 0 && s->tab + k != to;
    return (cells + 1) << empty;
}

static bool can_drop(const sol_t *s, int from, int i, int to) {
    const pile_t *a = &s->p[from], *b = &s->p[to];
    if (from == to || !can_pick(s, from, i))
        return false;
    int m = a->n - i;
    uint8_t c = a->c[i], t = top(b);
    switch (b->kind) {
    case K_FOUND:
        if (m != 1 || is_spider(s))
            return false;
        return b->n ? SUIT(t) == SUIT(c) && RANK(c) == RANK(t) + 1
                    : RANK(c) == 1;
    case K_CELL:
        return m == 1 && !b->n;
    case K_TAB:
        if (s->mode == FREECELL && m > capacity(s, to))
            return false;
        if (!b->n)
            return !is_klondike(s) || RANK(c) == 13;
        if (!(t & FACE) || RANK(t) != RANK(c) + 1)
            return false;
        return is_spider(s) || RED_SUIT(t) != RED_SUIT(c);
    default:
        return false;
    }
}

static int found_cards(const sol_t *s) {
    int n = 0;
    for (int k = 0; k < s->nfound; k++)
        n += s->p[s->found + k].n;
    return n;
}

static int total_cards(const sol_t *s) { return is_spider(s) ? 104 : 52; }

/* Spider: a whole suit, King to Ace, leaves for a foundation */
static void spider_runs(sol_t *s) {
    for (int k = 0; k < s->ntab; k++) {
        pile_t *p = &s->p[s->tab + k];
        if (p->n < 13 || RANK(top(p)) != 1 || !is_run(s, p, p->n - 13) ||
            RANK(p->c[p->n - 13]) != 13)
            continue;
        for (int f = 0; f < s->nfound; f++)
            if (!s->p[s->found + f].n) {
                move(s, s->tab + k, s->found + f, 13, U_CHAIN);
                break;
            }
    }
}

/* FreeCell: cards nothing could need any more go up by themselves */
static bool safe_up(const sol_t *s, uint8_t c) {
    if (RANK(c) <= 2)
        return true;
    for (int f = 0; f < s->nfound; f++) { /* the other colour's piles */
        const pile_t *p = &s->p[s->found + f];
        if (p->n && RED_SUIT(top(p)) == RED_SUIT(c))
            continue;
        if (!p->n || RANK(top(p)) < RANK(c) - 1)
            return false;
    }
    int other = 0; /* (both of them started) */
    for (int f = 0; f < s->nfound; f++)
        other += s->p[s->found + f].n &&
                 RED_SUIT(top(&s->p[s->found + f])) != RED_SUIT(c);
    return other == 2;
}

static int found_for(const sol_t *s, int from, int i) {
    for (int f = 0; f < s->nfound; f++)
        if (can_drop(s, from, i, s->found + f))
            return s->found + f;
    return -1;
}

static void auto_moves(sol_t *s) {
    bool any = true;
    /* Klondike: everything turned up, the stock gone: it finishes itself */
    bool finish = false;
    if (is_klondike(s) && !s->p[s->stock].n && !s->p[s->waste].n) {
        finish = true;
        for (int k = 0; k < s->ntab && finish; k++) {
            const pile_t *p = &s->p[s->tab + k];
            finish = !p->n || (p->c[0] & FACE);
        }
    }
    if (s->mode != FREECELL && !finish)
        return;
    while (any) {
        any = false;
        for (int pi = 0; pi < s->np; pi++) {
            pile_t *p = &s->p[pi];
            if ((p->kind != K_TAB && p->kind != K_CELL) || !p->n)
                continue;
            int to = found_for(s, pi, p->n - 1);
            if (to >= 0 && (finish || safe_up(s, top(p)))) {
                move(s, pi, to, 1, U_CHAIN);
                any = true;
            }
        }
    }
}

// the clock and the status bar

static uint32_t secs(const sol_t *s) {
    uint32_t ms = s->elapsed_ms;
    if (s->started && !s->won)
        ms += s->api->time_millis() - s->start_ms;
    return ms / 1000;
}

static void status(sol_t *s) {
    char b[96], extra[24] = "";
    int t = (int)secs(s);
    s->shown_secs = t;
    if (is_spider(s))
        s->api->snprintf(extra, sizeof extra, "  Deals %d",
                         s->p[s->stock].n / 10);
    else if (is_klondike(s))
        s->api->snprintf(extra, sizeof extra, "  Stock %d", s->p[s->stock].n);
    s->api->snprintf(b, sizeof b, "%s #%u%s  Moves %d  %d:%02d%s",
                     MODE_SHORT[s->mode], s->game, extra, s->moves, t / 60,
                     t % 60, s->won ? "  Won!" : "");
    s->api->ui->set_text(s->status, b);
}

static void redraw(sol_t *s) {
    s->api->invalidate(s->win);
    status(s);
}

// statistics (prefs: played, won, best seconds per mode)

static int stat(sol_t *s, char what, int mode) {
    char k[4] = {what, (char)('0' + mode), 0};
    return s->api->prefs_get_int(k, 0);
}

static void stat_set(sol_t *s, char what, int mode, int v) {
    char k[4] = {what, (char)('0' + mode), 0};
    s->api->prefs_set_int(k, v);
}

// dealing

static void deal(sol_t *s, int mode, uint32_t game) {
    const os_api_t *api = s->api;
    s->mode = mode;
    s->game = game;
    s->np = 0;
    s->nundo = s->moves = 0;
    s->won = s->started = false;
    s->elapsed_ms = 0;
    s->press_pile = -1;
    s->dragging = false;
    s->hint_until = 0;
    uint8_t deck[104];
    int n = 0;
    if (mode == FREECELL) { /* Microsoft's deals: #1 to #32000 and on */
        uint8_t left[52];
        for (int k = 0; k < 52; k++) /* AC AD AH AS 2C ... KS */
            left[k] = CARD(k / 4 + 1, k % 4);
        uint32_t seed = game;
        for (int k = 52; k > 0; k--) {
            seed = (seed * 214013u + 2531011u) & 0x7FFFFFFFu;
            int j = (int)((seed >> 16) % (uint32_t)k);
            deck[n++] = left[j];
            left[j] = left[k - 1];
        }
    } else {
        int suits = mode == SPIDER1 ? 1 : mode == SPIDER2 ? 2 : 4;
        int decks = is_spider(s) ? 2 : 1;
        for (int d = 0; d < decks * 4; d++) { /* 4 or 8 suits of 13 */
            int su = suits == 1   ? SPADES
                     : suits == 2 ? (d % 2 ? HEARTS : SPADES)
                                  : d % 4;
            for (int r = 1; r <= 13; r++)
                deck[n++] = CARD(r, su);
        }
        s->rng = game * 2654435761u ^ 0x9E3779B9u;
        for (int k = n - 1; k > 0; k--) {
            int j = (int)(rnd(s) % (uint32_t)(k + 1));
            uint8_t t = deck[k];
            deck[k] = deck[j];
            deck[j] = t;
        }
    }
    s->nfound = s->ncell = 0;
    s->stock = s->waste = -1;
    if (is_klondike(s)) {
        s->stock = add_pile(s, K_STOCK);
        s->waste = add_pile(s, K_WASTE);
        s->nfound = 4, s->ntab = 7;
    } else if (is_spider(s)) {
        s->stock = add_pile(s, K_STOCK);
        s->nfound = 8, s->ntab = 10;
    } else {
        s->ncell = 4, s->nfound = 4, s->ntab = 8;
        s->cell = s->np;
        for (int k = 0; k < 4; k++)
            add_pile(s, K_CELL);
    }
    s->found = s->np;
    for (int k = 0; k < s->nfound; k++)
        add_pile(s, K_FOUND);
    s->tab = s->np;
    for (int k = 0; k < s->ntab; k++)
        add_pile(s, K_TAB);

    int d = 0;
    if (mode == FREECELL) {
        for (int k = 0; k < 52; k++) {
            pile_t *p = &s->p[s->tab + k % 8];
            p->c[p->n++] = deck[d++] | FACE;
        }
    } else if (is_klondike(s)) {
        for (int k = 0; k < 7; k++)
            for (int r = k; r < 7; r++) {
                pile_t *p = &s->p[s->tab + r];
                p->c[p->n++] = (uint8_t)(deck[d++] | (r == k ? FACE : 0));
            }
    } else { /* Spider: 54 out, the first four columns one more */
        for (int k = 0; k < 54; k++) {
            pile_t *p = &s->p[s->tab + k % 10];
            p->c[p->n++] = deck[d++];
        }
        for (int k = 0; k < 10; k++)
            s->p[s->tab + k].c[s->p[s->tab + k].n - 1] |= FACE;
    }
    pile_t *st = s->stock >= 0 ? &s->p[s->stock] : NULL;
    while (st && d < n)
        st->c[st->n++] = deck[d++];
    api->prefs_set_int("mode", mode);
    s->lay_w = 0; /* (the layout again) */
    redraw(s);
}

static void new_game(sol_t *s, int mode) {
    uint32_t g = (rnd(s) ^ s->api->time_millis()) % 1000000u + 1;
    if (mode == FREECELL)
        g = g % 32000u + 1; /* (the classic numbers) */
    deal(s, mode, g);
}

// moving, as the player does

static void check_won(sol_t *s) {
    if (s->won || found_cards(s) != total_cards(s))
        return;
    s->won = true;
    s->elapsed_ms += s->api->time_millis() - s->start_ms;
    int t = (int)(s->elapsed_ms / 1000);
    stat_set(s, 'w', s->mode, stat(s, 'w', s->mode) + 1);
    int best = stat(s, 't', s->mode);
    bool record = !best || t < best;
    if (record)
        stat_set(s, 't', s->mode, t);
    s->api->file_write(SAVE_PATH, "", 0); /* (nothing to come back to) */
    redraw(s);
    char b[96];
    s->api->snprintf(b, sizeof b, "You won in %d moves, %d:%02d.%s", s->moves,
                     t / 60, t % 60, record ? "\nA new best time!" : "");
    s->api->ui->message(s->win, "Solitaire", b);
}

/* a move made: the clock starts, the game counts as played */
static void moved(sol_t *s) {
    if (!s->started) {
        s->started = true;
        s->start_ms = s->api->time_millis();
        stat_set(s, 'p', s->mode, stat(s, 'p', s->mode) + 1);
    }
    s->moves++;
    s->hint_until = 0;
    if (is_spider(s))
        spider_runs(s);
    auto_moves(s);
    check_won(s);
    redraw(s);
}

static bool try_move(sol_t *s, int from, int i, int to) {
    if (!can_drop(s, from, i, to))
        return false;
    move(s, from, to, s->p[from].n - i, 0);
    moved(s);
    return true;
}

/* the stock clicked: Klondike draws (or turns the waste over), Spider deals
 * a row */
static void stock_click(sol_t *s) {
    pile_t *st = &s->p[s->stock];
    if (is_klondike(s)) {
        if (st->n) {
            int k = s->mode == KLONDIKE3 ? 3 : 1;
            for (int j = 0; j < k && st->n; j++)
                move(s, s->stock, s->waste, 1, U_FLIP | (j ? U_CHAIN : 0));
        } else if (s->p[s->waste].n) {
            for (int j = 0; s->p[s->waste].n; j++)
                move(s, s->waste, s->stock, 1, U_FLIP | (j ? U_CHAIN : 0));
        } else {
            return;
        }
        moved(s);
        return;
    }
    if (!is_spider(s) || !st->n)
        return;
    for (int k = 0; k < s->ntab; k++)
        if (!s->p[s->tab + k].n) {
            s->api->ui->message(s->win, "Spider",
                                "Every column needs a card before a deal.");
            return;
        }
    for (int k = 0; k < s->ntab; k++)
        move(s, s->stock, s->tab + k, 1, U_FLIP | (k ? U_CHAIN : 0));
    moved(s);
}

/* a card clicked: to where it fits best (a foundation first, then a
 * column with cards, then an empty one or a free cell) */
static bool best_move(sol_t *s, int from, int i) {
    if (i == s->p[from].n - 1) {
        int f = found_for(s, from, i);
        if (f >= 0)
            return try_move(s, from, i, f);
    }
    int pick = -1, score = -1;
    for (int k = 0; k < s->ntab; k++) {
        int to = s->tab + k;
        if (!can_drop(s, from, i, to))
            continue;
        const pile_t *b = &s->p[to];
        int sc = b->n ? 2 : 1;
        if (b->n && is_spider(s) && SUIT(top(b)) == SUIT(s->p[from].c[i]))
            sc = 3; /* (the same suit: it can move on together) */
        if (!b->n && i == 0 && s->p[from].kind == K_TAB)
            sc = 0; /* (a whole column to an empty one: pointless) */
        if (sc > score)
            score = sc, pick = to;
    }
    if (score > 0)
        return try_move(s, from, i, pick);
    for (int k = 0; k < s->ncell; k++)
        if (can_drop(s, from, i, s->cell + k))
            return try_move(s, from, i, s->cell + k);
    return false;
}

static void undo(sol_t *s) {
    if (!s->nundo || s->won)
        return;
    step_t u;
    do {
        u = s->undo[--s->nundo];
        unmove(s, &u);
    } while ((u.flags & U_CHAIN) && s->nundo);
    if (s->moves)
        s->moves--;
    s->hint_until = 0;
    redraw(s);
}

/* a move worth making: false if there is none */
static bool find_hint(sol_t *s, int *from, int *idx, int *to) {
    int best = 0;
    for (int a = 0; a < s->np; a++) {
        const pile_t *p = &s->p[a];
        for (int i = 0; i < p->n; i++) {
            if (!can_pick(s, a, i) || p->kind == K_FOUND)
                continue;
            for (int b = 0; b < s->np; b++) {
                if (!can_drop(s, a, i, b))
                    continue;
                const pile_t *q = &s->p[b];
                int sc = 0;
                if (q->kind == K_FOUND)
                    sc = 6;
                else if (q->kind == K_TAB && q->n) {
                    sc = i > 0 && !(p->c[i - 1] & FACE) ? 5 /* turns one up */
                         : p->kind != K_TAB             ? 4
                                                        : 2;
                    if (is_spider(s) && SUIT(top(q)) == SUIT(p->c[i]))
                        sc++;
                    /* (onto the same rank it already sits on: no gain) */
                    if (p->kind == K_TAB && i > 0 && (p->c[i - 1] & FACE) &&
                        RANK(p->c[i - 1]) == RANK(top(q)) &&
                        (is_spider(s) ? SUIT(p->c[i - 1]) == SUIT(top(q))
                                      : true))
                        sc = 0;
                } else if (q->kind == K_TAB) {
                    sc = i > 0 ? 1 : 0;
                } else if (q->kind == K_CELL) {
                    sc = p->kind == K_TAB && p->n - i == 1 ? 1 : 0;
                }
                if (sc > best) {
                    best = sc;
                    *from = a, *idx = i, *to = b;
                }
            }
        }
    }
    return best > 0;
}

// layout: the card size from the window, the fan of each column

static void layout(sol_t *s, int w, int h) {
    int cols = s->ntab;
    s->lay_w = w, s->lay_h = h;
    s->gap = w >= 400 ? 4 : 2;
    int cw = (w - (cols + 1) * s->gap) / cols;
    cw = cw > 56 ? 56 : cw < 16 ? 16 : cw;
    s->cw = cw;
    s->ch = cw * 4 / 3;
    int left = (w - (cols * cw + (cols - 1) * s->gap)) / 2;
    int col_x[10];
    for (int k = 0; k < cols; k++)
        col_x[k] = left + k * (cw + s->gap);
    int ty = s->gap + 1, by = ty + s->ch + s->gap * 2 + 2;
    for (int k = 0; k < s->ntab; k++) {
        s->p[s->tab + k].x = (int16_t)col_x[k];
        s->p[s->tab + k].y = (int16_t)by;
    }
    if (is_klondike(s)) {
        s->p[s->stock].x = (int16_t)col_x[0];
        s->p[s->waste].x = (int16_t)col_x[1];
        for (int k = 0; k < 4; k++)
            s->p[s->found + k].x = (int16_t)col_x[3 + k];
    } else if (is_spider(s)) {
        s->p[s->stock].x = (int16_t)col_x[9];
        for (int k = 0; k < 8; k++) /* (overlapping) */
            s->p[s->found + k].x = (int16_t)(col_x[0] + k * cw / 3);
    } else {
        for (int k = 0; k < 4; k++) {
            s->p[s->cell + k].x = (int16_t)col_x[k];
            s->p[s->found + k].x = (int16_t)col_x[4 + k];
        }
    }
    for (int i = 0; i < s->np; i++)
        if (s->p[i].kind != K_TAB)
            s->p[i].y = (int16_t)ty;
}

/* each column's fan: as open as fits in the height */
static void fans(sol_t *s) {
    int fu0 = s->api->font_height(F_SMALL) + 4; /* (the corner shows) */
    if (fu0 < s->ch / 4)
        fu0 = s->ch / 4;
    for (int k = 0; k < s->ntab; k++) {
        pile_t *p = &s->p[s->tab + k];
        int nd = 0, avail = s->lay_h - p->y - s->gap;
        while (nd < p->n && !(p->c[nd] & FACE))
            nd++;
        int nu = p->n - nd, fd = s->ch / 10 < 2 ? 2 : s->ch / 10, fu = fu0;
        while (p->n && nd * fd + (nu - 1) * fu + s->ch > avail) {
            if (fu > 6)
                fu--;
            else if (fd > 1)
                fd--;
            else if (fu > 2)
                fu--;
            else
                break;
        }
        p->fd = (uint8_t)fd;
        p->fu = (uint8_t)fu;
    }
}

static int card_y(const pile_t *p, int k) {
    int y = p->y;
    for (int j = 0; j < k; j++)
        y += p->c[j] & FACE ? p->fu : p->fd;
    return y;
}

/* where card k of a pile is drawn */
static void card_xy(const sol_t *s, const pile_t *p, int k, int *x, int *y) {
    *x = p->x;
    *y = p->kind == K_TAB ? card_y(p, k) : p->y;
    if (p->kind == K_WASTE && s->mode == KLONDIKE3) { /* the last three */
        int first = p->n > 3 ? p->n - 3 : 0;
        if (k >= first)
            *x += (k - first) * (s->cw / 3);
    }
}

// drawing

static const char *const SUIT5[4][5] = {
    {"..#..", ".###.", "#####", "#.#.#", "..#.."},  /* clubs */
    {"..#..", ".###.", "#####", ".###.", "..#.."},  /* diamonds */
    {".#.#.", "#####", "#####", ".###.", "..#.."},  /* hearts */
    {"..#..", ".###.", "#####", "#####", "..#.."}}; /* spades */
static const char *const SUIT7[4][7] = {
    {"..###..", "..###..", "#.###.#", "#######", "#.#.#.#", "...#...",
     "..###.."},
    {"...#...", "..###..", ".#####.", "#######", ".#####.", "..###..",
     "...#..."},
    {".##.##.", "#######", "#######", "#######", ".#####.", "..###..",
     "...#..."},
    {"...#...", "..###..", ".#####.", "#######", "#######", "...#...",
     "..###.."}};

static void glyph(const os_api_t *api, int x, int y, int suit, bool big,
                  int scale, uint8_t col) {
    int n = big ? 7 : 5;
    for (int r = 0; r < n; r++)
        for (int c = 0; c < n; c++)
            if ((big ? SUIT7[suit][r] : SUIT5[suit][r])[c] == '#')
                api->fill_rect(x + c * scale, y + r * scale, scale, scale, col);
}

static const char *const RANK_TXT[14] = {"",  "A", "2", "3",  "4", "5", "6",
                                         "7", "8", "9", "10", "J", "Q", "K"};

static void draw_back(const sol_t *s, int x, int y) {
    const os_api_t *api = s->api;
    api->fill_rect(x, y, s->cw, s->ch, BLUE);
    api->draw_rect(x, y, s->cw, s->ch, BLACK);
    api->draw_rect(x + 1, y + 1, s->cw - 2, s->ch - 2, WHITE);
    for (int yy = y + 4; yy < y + s->ch - 3; yy += 4) /* a weave */
        api->fill_rect(x + 3, yy, s->cw - 6, 1, LIGHT_BLUE);
    for (int xx = x + 4; xx < x + s->cw - 3; xx += 4)
        api->fill_rect(xx, y + 3, 1, s->ch - 6, LIGHT_BLUE);
}

static void draw_card(const sol_t *s, int x, int y, uint8_t c) {
    const os_api_t *api = s->api;
    if (!(c & FACE)) {
        draw_back(s, x, y);
        return;
    }
    int cw = s->cw, ch = s->ch, r = RANK(c), su = SUIT(c);
    uint8_t col = RED_SUIT(c) ? RED : BLACK;
    api->fill_rect(x + 1, y + 1, cw - 2, ch - 2, WHITE);
    api->draw_rect(x, y, cw, ch, BLACK);
    const char *t = RANK_TXT[r];
    int fs = cw >= 40 ? F_BOLD : F_SMALL;
    int tw = api->text_width(t, fs), fh = api->font_height(fs);
    api->draw_string(x + 2, y + 2, t, col, fs);
    glyph(api, x + 3 + tw, y + 2 + (fh - 5) / 2, su, false, 1, col);
    /* the middle: the suit, big; a picture card's letter in a frame */
    int top = y + 3 + fh, room = y + ch - 3 - top;
    int sc = cw >= 44 ? 2 : 1, gs = 7 * sc;
    if (room < gs + 2)
        return;
    int gx = x + (cw - gs) / 2, gy = top + (room - gs) / 2;
    if (r >= 11) {
        int fw = cw - 8, fhh = room - 1;
        api->fill_rect(x + 4, top, fw, fhh, YELLOW);
        api->draw_rect(x + 4, top, fw, fhh, col);
        int bw = api->text_width(t, F_BOLD), bh = api->font_height(F_BOLD);
        if (fhh >= bh + gs + 4) { /* the letter over the suit */
            api->draw_string(x + (cw - bw) / 2, top + 2, t, col, F_BOLD);
            glyph(api, gx, top + fhh - gs - 2, su, true, sc, col);
        } else {
            api->draw_string(x + (cw - bw) / 2, top + (fhh - bh) / 2, t, col,
                             F_BOLD);
        }
        return;
    }
    glyph(api, gx, gy, su, true, sc, col);
}

static void draw_slot(const sol_t *s, int x, int y, const char *mark) {
    const os_api_t *api = s->api;
    api->draw_rect(x, y, s->cw, s->ch, LIGHT_GREEN);
    if (mark) {
        int tw = api->text_width(mark, F_BOLD);
        api->draw_string(x + (s->cw - tw) / 2,
                         y + (s->ch - api->font_height(F_BOLD)) / 2, mark,
                         LIGHT_GREEN, F_BOLD);
    }
}

static void outline(const sol_t *s, int x, int y, int h) {
    s->api->draw_rect(x - 1, y - 1, s->cw + 2, h + 2, YELLOW);
    s->api->draw_rect(x - 2, y - 2, s->cw + 4, h + 4, YELLOW);
}

/* the cards of pile pi that are shown (up to `upto`: the rest is dragged) */
static void draw_pile(const sol_t *s, int pi, int ox, int oy, int upto) {
    const pile_t *p = &s->p[pi];
    int x, y;
    if (upto == 0) {
        const char *mark =
            p->kind == K_FOUND                                         ? "A"
            : p->kind == K_STOCK && is_klondike(s) && s->p[s->waste].n ? "O"
                                                                       : NULL;
        if (p->kind == K_FOUND && is_spider(s))
            return; /* (an empty Spider foundation: nothing) */
        draw_slot(s, ox + p->x, oy + p->y, mark);
        return;
    }
    switch (p->kind) {
    case K_TAB:
        for (int k = 0; k < upto; k++) {
            card_xy(s, p, k, &x, &y);
            draw_card(s, ox + x, oy + y, p->c[k]);
        }
        break;
    case K_STOCK: /* a back per deal left (Spider) */
        if (is_spider(s)) {
            int deals = (upto + 9) / 10;
            for (int k = deals - 1; k >= 0; k--)
                draw_back(s, ox + p->x - k * 3, oy + p->y);
        } else {
            draw_back(s, ox + p->x, oy + p->y);
        }
        break;
    case K_WASTE: {
        int first = s->mode == KLONDIKE3 ? (upto > 3 ? upto - 3 : 0) : upto - 1;
        for (int k = first; k < upto; k++) {
            card_xy(s, p, k, &x, &y);
            draw_card(s, ox + x, oy + y, p->c[k]);
        }
        break;
    }
    case K_FOUND:
        /* Spider: the King of a finished suit; else the top card */
        draw_card(s, ox + p->x, oy + p->y,
                  is_spider(s) ? p->c[0] : p->c[upto - 1]);
        break;
    default:
        draw_card(s, ox + p->x, oy + p->y, p->c[upto - 1]);
        break;
    }
}

static void paint(control *c, int x, int y, int w, int h) {
    sol_t *s = c->user;
    const os_api_t *api = s->api;
    if (w != s->lay_w || h != s->lay_h)
        layout(s, w, h);
    fans(s);
    api->fill_rect(x, y, w, h, GREEN);
    bool drag = s->dragging && s->press_pile >= 0;
    for (int i = 0; i < s->np; i++) {
        if (s->p[i].kind == K_FOUND && is_spider(s) && s->p[i].n == 0)
            continue;
        int upto = drag && i == s->press_pile ? s->press_idx : s->p[i].n;
        draw_pile(s, i, x, y, upto);
    }
    if (is_spider(s) && !found_cards(s)) /* where finished suits go */
        draw_slot(s, x + s->p[s->found].x, y + s->p[s->found].y, NULL);
    if (s->hint_until && (int32_t)(api->time_millis() - s->hint_until) < 0) {
        const pile_t *a = &s->p[s->hint_from], *b = &s->p[s->hint_to];
        int ax, ay, bx, by;
        card_xy(s, a, s->hint_idx, &ax, &ay);
        int ah = a->kind == K_TAB ? card_y(a, a->n - 1) - ay + s->ch : s->ch;
        outline(s, x + ax, y + ay, ah);
        if (b->n)
            card_xy(s, b, b->n - 1, &bx, &by);
        else
            bx = b->x, by = b->y;
        outline(s, x + bx, y + by, s->ch);
    }
    if (drag) { /* the dragged cards, under the pointer */
        const pile_t *p = &s->p[s->press_pile];
        int dx = x + s->mx - s->grab_dx, dy = y + s->my - s->grab_dy;
        int step = p->kind == K_TAB ? p->fu : 0;
        for (int k = s->press_idx; k < p->n; k++)
            draw_card(s, dx, dy + (k - s->press_idx) * step, p->c[k]);
    }
}

// the mouse and the keys

/* the card under (mx, my): its pile and index (the pile's size: on its
 * empty place); false: none */
static bool hit(const sol_t *s, int mx, int my, int *pile, int *idx) {
    for (int i = s->np - 1; i >= 0; i--) {
        const pile_t *p = &s->p[i];
        if (p->kind == K_FOUND && is_spider(s))
            continue;
        if (!p->n || p->kind != K_TAB) {
            int k = p->n ? p->n - 1 : 0, x, y;
            card_xy(s, p, k, &x, &y);
            int x0 = p->x; /* (the fanned waste: from its left) */
            if (mx >= x0 && mx < x + s->cw && my >= y && my < y + s->ch) {
                *pile = i;
                *idx = p->n ? p->n - 1 : 0;
                return true;
            }
            continue;
        }
        for (int k = p->n - 1; k >= 0; k--) {
            int x, y;
            card_xy(s, p, k, &x, &y);
            if (mx >= x && mx < x + s->cw && my >= y && my < y + s->ch) {
                *pile = i;
                *idx = k;
                return true;
            }
        }
    }
    return false;
}

/* where the dragged cards land: the legal pile they cover the most */
static int drop_target(const sol_t *s) {
    int dx = s->mx - s->grab_dx, dy = s->my - s->grab_dy, best = -1, area = 0;
    for (int b = 0; b < s->np; b++) {
        if (!can_drop(s, s->press_pile, s->press_idx, b))
            continue;
        const pile_t *q = &s->p[b];
        int tx, ty;
        card_xy(s, q, q->n ? q->n - 1 : 0, &tx, &ty);
        int w = (dx < tx ? dx + s->cw - tx : tx + s->cw - dx);
        int h = (dy < ty ? dy + s->ch - ty : ty + s->ch - dy);
        if (w > 0 && h > 0 && w * h > area)
            area = w * h, best = b;
    }
    return best;
}

static void on_view(control *c, int ev) {
    sol_t *s = c->user;
    if (ev == EV_MOUSEDOWN || ev == EV_DBLCLICK) {
        int pi, idx;
        s->press_pile = -1;
        s->dragging = false;
        s->mx = s->down_x = c->mx;
        s->my = s->down_y = c->my;
        if (s->won || !hit(s, c->mx, c->my, &pi, &idx))
            return;
        if (s->p[pi].kind == K_STOCK) {
            s->press_pile = pi;
            s->press_idx = -1;
        } else if (can_pick(s, pi, idx)) {
            int x, y;
            card_xy(s, &s->p[pi], idx, &x, &y);
            s->press_pile = pi;
            s->press_idx = idx;
            s->grab_dx = c->mx - x;
            s->grab_dy = c->my - y;
        }
        return;
    }
    if (ev == EV_MOUSEMOVE) {
        if (s->press_pile < 0 || s->press_idx < 0)
            return;
        s->mx = c->mx, s->my = c->my;
        int ddx = s->mx - s->down_x, ddy = s->my - s->down_y;
        if (ddx * ddx + ddy * ddy > 9)
            s->dragging = true;
        if (s->dragging)
            s->api->invalidate(s->win);
        return;
    }
    if (ev == EV_MOUSEUP) {
        int pi = s->press_pile, idx = s->press_idx;
        bool drag = s->dragging;
        s->press_pile = -1;
        s->dragging = false;
        if (pi < 0)
            return;
        if (drag) {
            s->mx = c->mx, s->my = c->my;
            s->press_pile = pi; /* (drop_target reads it) */
            int to = drop_target(s);
            s->press_pile = -1;
            if (to >= 0)
                try_move(s, pi, idx, to);
        } else if (s->p[pi].kind == K_STOCK) {
            stock_click(s);
        } else if (s->p[pi].kind != K_FOUND) {
            best_move(s, pi, idx);
        }
        s->api->invalidate(s->win);
        return;
    }
    if (ev != EV_KEY)
        return;
    uint8_t sc = kb.last_scancode;
    char ch = kb.last_char;
    if (kb.ctrl && (sc == 0x2C || kb.ctrl_z))
        undo(s);
    else if (sc == F2)
        new_game(s, s->mode);
    else if (ch == ' ' || sc == ENTER)
        stock_click(s);
    else if (ch == 'h' || ch == 'H') {
        int a, i, b;
        if (find_hint(s, &a, &i, &b)) {
            s->hint_from = a, s->hint_idx = i, s->hint_to = b;
            s->hint_until = s->api->time_millis() + 1500;
            s->api->invalidate(s->win);
        }
    }
}

// menus

static void m_new(void *st) {
    sol_t *s = st;
    new_game(s, s->mode);
}
static void m_k1(void *st) { new_game(st, KLONDIKE1); }
static void m_k3(void *st) { new_game(st, KLONDIKE3); }
static void m_s1(void *st) { new_game(st, SPIDER1); }
static void m_s2(void *st) { new_game(st, SPIDER2); }
static void m_s4(void *st) { new_game(st, SPIDER4); }
static void m_fc(void *st) { new_game(st, FREECELL); }

static void m_restart(void *st) {
    sol_t *s = st;
    deal(s, s->mode, s->game);
}

static void picked_game(void *user, const char *text) {
    sol_t *s = user;
    if (!text)
        return;
    uint32_t n = 0;
    for (const char *p = text; *p >= '0' && *p <= '9' && n < 10000000; p++)
        n = n * 10 + (uint32_t)(*p - '0');
    if (n < 1 || n > 1000000) {
        s->api->ui->message(s->win, "Select Game",
                            "A number from 1 to 1000000.");
        return;
    }
    deal(s, s->mode, n);
}

static void m_select(void *st) {
    sol_t *s = st;
    char b[12];
    s->api->snprintf(b, sizeof b, "%u", s->game);
    s->api->ui->input(s->win, "Select Game", "Game number (1-1000000):", b, 7,
                      ES_NUMBER, picked_game, s);
}

static void m_undo(void *st) { undo(st); }

static void m_hint(void *st) {
    sol_t *s = st;
    int a, i, b;
    if (find_hint(s, &a, &i, &b)) {
        s->hint_from = a, s->hint_idx = i, s->hint_to = b;
        s->hint_until = s->api->time_millis() + 1500;
        s->api->invalidate(s->win);
    } else {
        s->api->ui->message(s->win, "Hint",
                            s->stock >= 0 && s->p[s->stock].n
                                ? "No moves on the table: deal from the stock."
                                : "No moves left.");
    }
}

static void m_deal(void *st) { stock_click(st); }

static void m_stats(void *st) {
    sol_t *s = st;
    char b[400], *p = b;
    int n = sizeof b;
    for (int m = 0; m < MODES; m++) {
        int pl = stat(s, 'p', m), w = stat(s, 'w', m), t = stat(s, 't', m);
        int k = s->api->snprintf(p, (size_t)n, "%s: %d of %d won", MODE_NAME[m],
                                 w, pl);
        p += k, n -= k;
        if (t) {
            k = s->api->snprintf(p, (size_t)n, ", best %d:%02d", t / 60,
                                 t % 60);
            p += k, n -= k;
        }
        k = s->api->snprintf(p, (size_t)n, "\n");
        p += k, n -= k;
    }
    s->api->ui->message(s->win, "Statistics", b);
}

static void m_close(void *st) {
    sol_t *s = st;
    s->api->quit_self(s);
}

static void m_help(void *st) {
    sol_t *s = st;
    const char *t;
    if (is_klondike(s))
        t = "Build the four suits up from Ace to King\n"
            "on the top right. Columns go down in\n"
            "alternate colours; only a King goes on an\n"
            "empty one. Click the stock to turn cards.";
    else if (is_spider(s))
        t = "Build each suit down from King to Ace in\n"
            "the columns: a whole suit then leaves.\n"
            "Any card goes on one a rank higher, but\n"
            "only a run of one suit moves together.\n"
            "Click the stock for ten more cards.";
    else
        t = "All cards are dealt face up. Build the\n"
            "suits up from Ace on the right; columns go\n"
            "down in alternate colours. The four cells\n"
            "on the left hold a card each.";
    s->api->ui->message(s->win, MODE_NAME[s->mode], t);
}

static void m_keys(void *st) {
    sol_t *s = st;
    s->api->ui->message(s->win, "Mouse and Keys",
                        "Drag cards, or click one: it goes where it\n"
                        "fits best.\n\n"
                        "F2 new game, Ctrl+Z undo, H hint,\n"
                        "Space deals from the stock.");
}

static void m_about(void *st) {
    sol_t *s = st;
    s->api->ui->message(s->win, "About Solitaire",
                        "Solitaire 1.0 for ASMOS\n"
                        "Klondike, Spider and FreeCell.");
}

// the game in progress, kept (a file: the piles are longer than prefs)

typedef struct {
    uint32_t magic;
    uint32_t game, elapsed_ms;
    int32_t moves;
    uint8_t mode, np;
    uint8_t n[NPILES];
    uint8_t c[NPILES][MAXC];
} save_t;

static save_t s_save; /* (too big for the stack of a callback) */

static void save_game(sol_t *s) {
    if (!s->started || s->won) {
        s->api->remove(SAVE_PATH);
        return;
    }
    save_t *v = &s_save;
    v->magic = SAVE_MAGIC;
    v->game = s->game;
    v->elapsed_ms = s->elapsed_ms + (s->api->time_millis() - s->start_ms);
    v->moves = s->moves;
    v->mode = (uint8_t)s->mode;
    v->np = (uint8_t)s->np;
    for (int i = 0; i < s->np; i++) {
        v->n[i] = s->p[i].n;
        s->api->mem_copy(v->c[i], s->p[i].c, MAXC);
    }
    s->api->file_write(SAVE_PATH, v, sizeof *v);
}

/* the same cards as a fresh deal of that game, or it's not taken */
static bool load_game(sol_t *s) {
    save_t *v = &s_save;
    if (s->api->file_read(SAVE_PATH, v, sizeof *v) != (int)sizeof *v ||
        v->magic != SAVE_MAGIC || v->mode >= MODES)
        return false;
    deal(s, v->mode, v->game);
    if (v->np != s->np)
        return false;
    uint8_t want[64], got[64];
    s->api->mem_set(want, 0, sizeof want);
    s->api->mem_set(got, 0, sizeof got);
    for (int i = 0; i < s->np; i++) {
        if (v->n[i] > MAXC)
            return false;
        for (int k = 0; k < s->p[i].n; k++)
            want[s->p[i].c[k] & 63]++;
        for (int k = 0; k < v->n[i]; k++)
            got[v->c[i][k] & 63]++;
    }
    for (int k = 0; k < 64; k++)
        if (want[k] != got[k])
            return false;
    for (int i = 0; i < s->np; i++) {
        s->p[i].n = v->n[i];
        s->api->mem_copy(s->p[i].c, v->c[i], MAXC);
    }
    s->moves = v->moves;
    s->elapsed_ms = v->elapsed_ms;
    s->started = true;
    s->start_ms = s->api->time_millis();
    redraw(s);
    return true;
}

// entry points (shim.c calls them; A is the API)

static void so_init(void *state) {
    sol_t *s = state;
    const os_api_t *api = s->api = A;
    const ui_api_t *ui = api->ui;
    int sw, sh;
    api->screen_size(&sw, &sh);
    int w = sw >= 640 ? 600 : sw - 6, h = sh >= 400 ? 440 : sh - 14;
    s->win = ui->app_window(s, "Solitaire", (sw - w) / 2, 12, w, h, LIGHT_GRAY,
                            AW_RESIZABLE);
    if (!s->win)
        return;
    int cx, cy, cw, ch;
    ui->client_rect(s->win, &cx, &cy, &cw, &ch);
    s->view = ui->custom(s->win, 0, 0, cw, ch - STATUS_H, paint, on_view,
                         CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);
    s->status = ui->status(s->win, "");
    api->set_lazy(s->win, true);

    menu *g = ui->add_menu(s->win, "Game");
    ui->menu_item(g, "New Game  F2", m_new);
    ui->menu_item(g, "Restart This Deal", m_restart);
    ui->menu_item(g, "Select Game...", m_select);
    menu *t = api->submenu(g, "Game Type");
    ui->menu_item(t, "Klondike (Draw 1)", m_k1);
    ui->menu_item(t, "Klondike (Draw 3)", m_k3);
    ui->menu_item(t, "Spider (1 Suit)", m_s1);
    ui->menu_item(t, "Spider (2 Suits)", m_s2);
    ui->menu_item(t, "Spider (4 Suits)", m_s4);
    ui->menu_item(t, "FreeCell", m_fc);
    ui->menu_separator(g);
    ui->menu_item(g, "Statistics...", m_stats);
    ui->menu_separator(g);
    ui->menu_item(g, "Close", m_close);
    menu *e = ui->add_menu(s->win, "Edit");
    ui->menu_item(e, "Undo  Ctrl+Z", m_undo);
    ui->menu_item(e, "Hint  H", m_hint);
    ui->menu_item(e, "Deal  Space", m_deal);
    menu *hm = ui->add_menu(s->win, "Help");
    ui->menu_item(hm, "How to Play", m_help);
    ui->menu_item(hm, "Mouse and Keys", m_keys);
    ui->menu_item(hm, "About Solitaire", m_about);

    s->rng = api->time_millis() | 1;
    s->press_pile = -1;
    if (!load_game(s))
        new_game(s, api->prefs_get_int("mode", KLONDIKE1) % MODES);
    ui->focus(s->view);
}

static void so_frame(void *state) {
    sol_t *s = state;
    if (!s->win)
        return;
    if (s->hint_until &&
        (int32_t)(s->api->time_millis() - s->hint_until) >= 0) {
        s->hint_until = 0;
        s->api->invalidate(s->win);
    }
    if (s->started && !s->won && (int)secs(s) != s->shown_secs)
        status(s);
}

static void so_destroy(void *state) {
    sol_t *s = state;
    if (s->win)
        save_game(s);
}

APP_META("Solitaire", sol_t, true, 0, so_init, so_frame, so_destroy);
