/* Sudoku - a bundled ASMOS app (/APPS/Games/Sudoku.APP).
 *
 * Fill the grid so every row, column and 3x3 box has 1 to 9 once.
 *   click / arrows   select a cell
 *   1-9              enter a digit       Shift+1-9  pencil a note
 *   N                notes mode on/off   Del / Backspace / 0 / Space  clear
 *   Ctrl+Z           undo
 * Each puzzle has exactly one solution. The game in progress, the best
 * times and the last difficulty are kept in /PREFS/SUDOKU.INI.
 *
 * Built by make (EXT_APPS, extapps/MANIFEST); the stand-alone version,
 * built with fddmaker, is testapps/sudoku.c.
 */
#include "extapps/shim.h"

/* cell size, pixels: the same on every screen (board 113 px, fits 320x200;
 * Settings > System > Large display makes it bigger) */
static int s_cs = 11;
#define CS s_cs
#define OFF(i) (2 + (i) * (CS + 1) + (i) / 3)
#define BOARD OFF(9) /* cells + thin and thick lines */
#define STATUS_H 16
#define UNDO_MAX 200

/* the font sizes of draw_string */
#define F_SMALL 2 /* 4x6, for notes */
#define F_BOLD 3

enum { EASY, MEDIUM, HARD, LEVELS };
static const char *const LEVEL_NAME[LEVELS] = {"Easy", "Medium", "Hard"};
static const int LEVEL_HOLES[LEVELS] = {40, 48, 56};

typedef struct {
    uint8_t cell, was; /* cell, its old digit */
    uint16_t notes;    /* its old notes */
} undo_t;

typedef struct {
    window *win; /* must stay first (--window-first) */
    const os_api_t *api;
    control *view, *status;
    uint8_t sol[81];    /* the solution */
    uint8_t given[81];  /* 1 = part of the puzzle */
    uint8_t cur[81];    /* 0 = empty */
    uint16_t notes[81]; /* bit d = note d */
    uint8_t wrong[81];  /* marked by Check, cleared on the next edit */
    int sel, level;
    bool notes_mode, solved, playing;
    uint32_t start_ms, elapsed_ms; /* elapsed: banked before a pause */
    int shown_secs;
    int best[LEVELS]; /* seconds, 0 = none */
    undo_t undo[UNDO_MAX];
    int nundo;
    uint32_t rng;
} sudoku_t;

/* ── random numbers (xorshift32) ── */
static uint32_t rnd(sudoku_t *s) {
    uint32_t x = s->rng ? s->rng : 0x2545F491u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s->rng = x;
}

static void shuffle9(sudoku_t *s, uint8_t *d) {
    for (int i = 0; i < 9; i++)
        d[i] = (uint8_t)(i + 1);
    for (int i = 8; i > 0; i--) {
        int j = (int)(rnd(s) % (uint32_t)(i + 1));
        uint8_t t = d[i];
        d[i] = d[j];
        d[j] = t;
    }
}

/* ── solver: bitmasks + fewest-candidates-first ── */
typedef struct {
    uint8_t g[81];
    uint16_t row[9], col[9], box[9]; /* bit d = digit d used */
} grid_t;

#define BOX(i) ((i) / 27 * 3 + (i) % 9 / 3)

static bool grid_load(grid_t *g, const uint8_t *src) {
    for (int i = 0; i < 9; i++)
        g->row[i] = g->col[i] = g->box[i] = 0;
    for (int i = 0; i < 81; i++) {
        int d = g->g[i] = src[i];
        if (!d)
            continue;
        uint16_t b = (uint16_t)(1u << d);
        if ((g->row[i / 9] | g->col[i % 9] | g->box[BOX(i)]) & b)
            return false;
        g->row[i / 9] |= b;
        g->col[i % 9] |= b;
        g->box[BOX(i)] |= b;
    }
    return true;
}

static int popcount9(uint16_t m) {
    int n = 0;
    for (m >>= 1; m; m >>= 1)
        n += m & 1;
    return n;
}

/* Count solutions up to `limit`. With `rs` set, digits are tried in a
 * random order (to make a full random grid) and the grid keeps the first
 * solution found. */
static int solve(grid_t *g, int limit, sudoku_t *rs) {
    int best = -1, bestn = 10;
    uint16_t bestfree = 0;
    for (int i = 0; i < 81; i++) {
        if (g->g[i])
            continue;
        uint16_t free =
            (uint16_t)(~(g->row[i / 9] | g->col[i % 9] | g->box[BOX(i)]) &
                       0x3FE);
        int n = popcount9(free);
        if (n < bestn) {
            best = i, bestn = n, bestfree = free;
            if (n <= 1)
                break;
        }
    }
    if (best < 0)
        return 1; /* full */
    if (!bestn)
        return 0;
    uint8_t order[9];
    if (rs)
        shuffle9(rs, order);
    else
        for (int k = 0; k < 9; k++)
            order[k] = (uint8_t)(k + 1);
    int found = 0, r = best / 9, c = best % 9, bx = BOX(best);
    for (int k = 0; k < 9 && found < limit; k++) {
        int d = order[k];
        uint16_t b = (uint16_t)(1u << d);
        if (!(bestfree & b))
            continue;
        g->g[best] = (uint8_t)d;
        g->row[r] |= b, g->col[c] |= b, g->box[bx] |= b;
        found += solve(g, limit - found, rs);
        if (rs && found)
            return found; /* keep the grid */
        g->row[r] &= (uint16_t)~b, g->col[c] &= (uint16_t)~b,
            g->box[bx] &= (uint16_t)~b;
        g->g[best] = 0;
    }
    return found;
}

static int count_solutions(const uint8_t *p, int limit) {
    grid_t g;
    if (!grid_load(&g, p))
        return 0;
    return solve(&g, limit, NULL);
}

/* ── game state ── */
static uint32_t now_secs(sudoku_t *s) {
    uint32_t ms = s->elapsed_ms;
    if (s->playing && !s->solved)
        ms += s->api->time_millis() - s->start_ms;
    return ms / 1000;
}

static void status(sudoku_t *s) {
    char b[64];
    int t = (int)now_secs(s);
    s->shown_secs = t;
    if (s->solved)
        s->api->snprintf(b, sizeof b, "Solved! %s in %d:%02d",
                         LEVEL_NAME[s->level], t / 60, t % 60);
    else
        s->api->snprintf(b, sizeof b, "%s  %d:%02d%s", LEVEL_NAME[s->level],
                         t / 60, t % 60, s->notes_mode ? "  NOTES" : "");
    s->api->ui->set_text(s->status, b);
}

static void redraw(sudoku_t *s) {
    s->api->invalidate(s->win);
    status(s);
}

static void new_game(sudoku_t *s, int level) {
    const os_api_t *api = s->api;
    s->rng ^= api->time_millis() * 2654435761u;
    grid_t g;
    uint8_t zero[81];
    api->mem_set(zero, 0, sizeof zero); /* no libc memset here */
    grid_load(&g, zero);
    solve(&g, 1, s);
    api->mem_copy(s->sol, g.g, 81);

    /* take out cells in symmetric pairs while the solution stays unique */
    uint8_t p[81], order[41];
    api->mem_copy(p, s->sol, 81);
    for (int i = 0; i <= 40; i++)
        order[i] = (uint8_t)i;
    for (int i = 40; i > 0; i--) {
        int j = (int)(rnd(s) % (uint32_t)(i + 1));
        uint8_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    int holes = 0;
    for (int k = 0; k <= 40 && holes < LEVEL_HOLES[level]; k++) {
        int a = order[k], b = 80 - a;
        uint8_t va = p[a], vb = p[b];
        p[a] = p[b] = 0;
        if (count_solutions(p, 2) == 1)
            holes += a == b ? 1 : 2;
        else
            p[a] = va, p[b] = vb;
    }

    for (int i = 0; i < 81; i++) {
        s->cur[i] = p[i];
        s->given[i] = p[i] != 0;
        s->notes[i] = 0;
        s->wrong[i] = 0;
    }
    s->level = level;
    s->sel = 40;
    s->solved = false;
    s->playing = true;
    s->nundo = 0;
    s->elapsed_ms = 0;
    s->start_ms = api->time_millis();
    api->prefs_set_int("level", level);
    redraw(s);
}

static bool check_solved(sudoku_t *s) {
    for (int i = 0; i < 81; i++)
        if (s->cur[i] != s->sol[i])
            return false;
    return true;
}

/* Prefs values are at most 95 characters: the solution and the board go
 * in two keys of 81. Board: 'G' = given, '0'..'9' = entered. */
static void save_game(sudoku_t *s) {
    char sol[82], cur[82];
    for (int i = 0; i < 81; i++) {
        sol[i] = (char)('0' + s->sol[i]);
        cur[i] = (char)(s->given[i] ? 'G' : '0' + s->cur[i]);
    }
    sol[81] = cur[81] = 0;
    s->api->prefs_set("sol", s->solved ? "" : sol);
    s->api->prefs_set("cur", s->solved ? "" : cur);
    s->api->prefs_set_int("time", (int)now_secs(s));
}

static bool load_game(sudoku_t *s) {
    char sol[96], cur[96];
    if (!s->api->prefs_get("sol", sol, sizeof sol) ||
        !s->api->prefs_get("cur", cur, sizeof cur))
        return false;
    for (int i = 0; i < 81; i++)
        if (!sol[i] || !cur[i])
            return false;
    for (int i = 0; i < 81; i++) {
        s->sol[i] = (uint8_t)(sol[i] - '0');
        s->given[i] = cur[i] == 'G';
        s->cur[i] = s->given[i] ? s->sol[i] : (uint8_t)(cur[i] - '0');
        if (s->sol[i] < 1 || s->sol[i] > 9 || s->cur[i] > 9)
            return false;
    }
    grid_t g;
    if (!grid_load(&g, s->sol))
        return false;
    s->level = s->api->prefs_get_int("level", EASY) % LEVELS;
    s->elapsed_ms = (uint32_t)s->api->prefs_get_int("time", 0) * 1000;
    s->start_ms = s->api->time_millis();
    s->playing = true;
    s->sel = 40;
    return true;
}

static void set_cell(sudoku_t *s, int i, int d, bool note) {
    if (s->solved || s->given[i])
        return;
    if (note && d && s->cur[i])
        return;
    if (s->nundo == UNDO_MAX) {
        for (int k = 1; k < UNDO_MAX; k++)
            s->undo[k - 1] = s->undo[k];
        s->nundo--;
    }
    s->undo[s->nundo++] = (undo_t){(uint8_t)i, s->cur[i], s->notes[i]};
    for (int k = 0; k < 81; k++)
        s->wrong[k] = 0;
    if (note) {
        if (d)
            s->notes[i] ^= (uint16_t)(1u << d);
        else
            s->notes[i] = 0;
    } else {
        s->cur[i] = (uint8_t)d;
        if (!d)
            s->notes[i] = 0;
        else /* clear this digit from the notes it can no longer be */
            for (int k = 0; k < 81; k++)
                if (k / 9 == i / 9 || k % 9 == i % 9 || BOX(k) == BOX(i))
                    s->notes[k] &= (uint16_t)~(1u << d);
    }
    if (check_solved(s)) {
        s->elapsed_ms += s->api->time_millis() - s->start_ms;
        s->solved = true;
        int t = (int)(s->elapsed_ms / 1000);
        if (!s->best[s->level] || t < s->best[s->level]) {
            s->best[s->level] = t;
            char k[8];
            s->api->snprintf(k, sizeof k, "best%d", s->level);
            s->api->prefs_set_int(k, t);
        }
        s->api->prefs_set("sol", "");
        s->api->prefs_set("cur", "");
        redraw(s);
        char b[64];
        s->api->snprintf(b, sizeof b, "Solved in %d:%02d.%s", t / 60, t % 60,
                         s->best[s->level] == t ? "\nA new best time!" : "");
        s->api->ui->message(s->win, "Sudoku", b);
        return;
    }
    redraw(s);
}

static void undo(sudoku_t *s) {
    if (!s->nundo || s->solved)
        return;
    undo_t u = s->undo[--s->nundo];
    s->cur[u.cell] = u.was;
    s->notes[u.cell] = u.notes;
    s->sel = u.cell;
    redraw(s);
}

/* ── drawing ── */
static void paint(control *c, int x, int y, int w, int h) {
    sudoku_t *s = c->user;
    const os_api_t *api = s->api;
    api->fill_rect(x, y, w, h, LIGHT_GRAY);
    int bx = x + (w - BOARD) / 2, by = y + (h - BOARD) / 2;
    api->fill_rect(bx, by, BOARD, BOARD, BLACK);
    int sd = s->cur[s->sel];
    for (int i = 0; i < 81; i++) {
        int r = i / 9, col = i % 9;
        int cx = bx + OFF(col), cy = by + OFF(r);
        int d = s->cur[i];
        uint8_t bg = WHITE;
        if (i == s->sel)
            bg = s->notes_mode ? LIGHT_GREEN : YELLOW;
        else if (s->wrong[i])
            bg = LIGHT_RED;
        else if (sd && d == sd)
            bg = LIGHT_CYAN;
        api->fill_rect(cx, cy, CS, CS, bg);
        if (d) {
            char t[2] = {(char)('0' + d), 0};
            uint8_t fg = s->given[i] ? BLACK : s->wrong[i] ? RED : BLUE;
            int tw = api->text_width(t, F_BOLD);
            api->draw_string(cx + (CS - tw) / 2 + 1,
                             cy + (CS - api->font_height(F_BOLD)) / 2 + 1, t,
                             fg, F_BOLD);
        } else if (s->notes[i]) {
            for (int n = 1; n <= 9; n++) {
                if (!(s->notes[i] & (1u << n)))
                    continue;
                if (CS >= 15) /* the digit, in its place in a 3x3 */
                    api->draw_char(cx + 1 + (n - 1) % 3 * 5,
                                   cy + (n - 1) / 3 * 5, (char)('0' + n),
                                   DARK_GRAY, F_SMALL);
                else /* small cells: a dot in the digit's place */
                    api->fill_rect(cx + 2 + (n - 1) % 3 * 3,
                                   cy + 2 + (n - 1) / 3 * 3, 2, 2, DARK_GRAY);
            }
        }
    }
}

/* ── input ── */
static void on_view(control *c, int ev) {
    sudoku_t *s = c->user;
    const keyboardvar *kbd = &kb; /* shim.h: the API's */
    if (ev == EV_MOUSEDOWN || ev == EV_DBLCLICK) {
        int bx = (c->aw - BOARD) / 2, by = (c->ah - BOARD) / 2;
        int col = -1, row = -1;
        for (int i = 0; i < 9; i++) {
            if (c->mx >= bx + OFF(i) && c->mx < bx + OFF(i) + CS + 1)
                col = i;
            if (c->my >= by + OFF(i) && c->my < by + OFF(i) + CS + 1)
                row = i;
        }
        if (col >= 0 && row >= 0) {
            s->sel = row * 9 + col;
            s->api->invalidate(s->win);
        }
        return;
    }
    if (ev != EV_KEY)
        return;
    uint8_t sc = kbd->last_scancode;
    char ch = kbd->last_char;
    int r = s->sel / 9, col = s->sel % 9;
    if (kbd->ctrl && (sc == 0x2C || kbd->ctrl_z)) { /* Ctrl+Z */
        undo(s);
        return;
    }
    switch (sc) {
    case LEFT_ARROW:
        col = (col + 8) % 9;
        break;
    case RIGHT_ARROW:
        col = (col + 1) % 9;
        break;
    case UP_ARROW:
        r = (r + 8) % 9;
        break;
    case DOWN_ARROW:
        r = (r + 1) % 9;
        break;
    case DELETE:
    case BACKSPACE:
        set_cell(s, s->sel, 0, false);
        return;
    default:
        /* 1-9 by scancode (0x02..0x0A), so Shift+digit still works */
        if (sc >= 0x02 && sc <= 0x0A) {
            set_cell(s, s->sel, sc - 1, kbd->shift || s->notes_mode);
            return;
        }
        if (ch >= '1' && ch <= '9') { /* keypad */
            set_cell(s, s->sel, ch - '0', s->notes_mode);
            return;
        }
        if (ch == '0' || ch == ' ' || sc == 0x0B) {
            set_cell(s, s->sel, 0, false);
            return;
        }
        if (ch == 'n' || ch == 'N') {
            s->notes_mode = !s->notes_mode;
            redraw(s);
        }
        return;
    }
    s->sel = r * 9 + col;
    s->api->invalidate(s->win);
}

/* ── menus ── */
static void m_easy(void *st) { new_game(st, EASY); }
static void m_medium(void *st) { new_game(st, MEDIUM); }
static void m_hard(void *st) { new_game(st, HARD); }

static void m_restart(void *st) {
    sudoku_t *s = st;
    for (int i = 0; i < 81; i++)
        if (!s->given[i])
            s->cur[i] = 0, s->notes[i] = 0, s->wrong[i] = 0;
    s->solved = false;
    s->nundo = 0;
    redraw(s);
}

static void m_undo(void *st) { undo(st); }

static void m_notes(void *st) {
    sudoku_t *s = st;
    s->notes_mode = !s->notes_mode;
    redraw(s);
}

static void m_check(void *st) {
    sudoku_t *s = st;
    int n = 0;
    for (int i = 0; i < 81; i++) {
        s->wrong[i] = s->cur[i] && s->cur[i] != s->sol[i];
        n += s->wrong[i];
    }
    redraw(s);
    char b[48];
    if (n)
        s->api->snprintf(b, sizeof b, "%d wrong digit%s, marked in red.", n,
                         n == 1 ? "" : "s");
    else
        s->api->snprintf(b, sizeof b, "No mistakes so far.");
    s->api->ui->message(s->win, "Check", b);
}

static void m_hint(void *st) {
    sudoku_t *s = st;
    int i = s->sel;
    if (s->solved)
        return;
    if (s->given[i] || s->cur[i] == s->sol[i]) /* pick an empty cell */
        for (int k = 0; k < 81; k++)
            if (!s->cur[(i + k) % 81]) {
                i = (i + k) % 81;
                break;
            }
    s->sel = i;
    s->start_ms -= 30000; /* a hint costs 30 seconds */
    set_cell(s, i, s->sol[i], false);
}

static void m_best(void *st) {
    sudoku_t *s = st;
    char b[128], *p = b;
    int n = sizeof b;
    for (int l = 0; l < LEVELS; l++) {
        int k =
            s->best[l]
                ? s->api->snprintf(p, (size_t)n, "%s: %d:%02d\n", LEVEL_NAME[l],
                                   s->best[l] / 60, s->best[l] % 60)
                : s->api->snprintf(p, (size_t)n, "%s: -\n", LEVEL_NAME[l]);
        p += k, n -= k;
    }
    s->api->ui->message(s->win, "Best Times", b);
}

static void m_close(void *st) {
    sudoku_t *s = st;
    s->api->quit_self(s);
}

static void m_help(void *st) {
    sudoku_t *s = st;
    s->api->ui->message(s->win, "How to Play",
                        "Fill the grid: each row, column and 3x3 box\n"
                        "has the digits 1 to 9 once.\n\n"
                        "1-9 enter, Shift+1-9 note, N notes mode,\n"
                        "Del clear, arrows move, Ctrl+Z undo.\n"
                        "A hint costs 30 seconds.");
}

static void m_about(void *st) {
    sudoku_t *s = st;
    s->api->ui->message(s->win, "About Sudoku",
                        "Sudoku 1.0 for ASMOS\n"
                        "Every puzzle has one solution.");
}

/* ── entry points (shim.c calls them; A is the API) ── */
static void sd_init(void *state) {
    sudoku_t *s = state;
    const os_api_t *api = s->api = A;
    const ui_api_t *ui = api->ui;
    int cw = BOARD + 8, ch = BOARD + 8 + STATUS_H;
    s->win = ui->app_window(s, "Sudoku", 20, 13, cw + 2, ch + 10, DARK_GRAY, 0);
    if (!s->win)
        return;
    s->view = ui->custom(s->win, 0, 0, cw, BOARD + 8, paint, on_view,
                         CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);
    s->status = ui->status(s->win, "");
    api->set_lazy(s->win, true); /* the board changes in handlers only */

    menu *g = ui->add_menu(s->win, "Game");
    menu *ng = api->submenu(g, "New Game"); /* Easy / Medium / Hard */
    ui->menu_item(ng, "Easy", m_easy);
    ui->menu_item(ng, "Medium", m_medium);
    ui->menu_item(ng, "Hard", m_hard);
    ui->menu_item(g, "Restart", m_restart);
    ui->menu_separator(g);
    ui->menu_item(g, "Best Times...", m_best);
    ui->menu_separator(g);
    ui->menu_item(g, "Close", m_close);
    menu *e = ui->add_menu(s->win, "Edit");
    ui->menu_item(e, "Undo  Ctrl+Z", m_undo);
    ui->menu_item(e, "Notes Mode  N", m_notes);
    ui->menu_separator(e);
    ui->menu_item(e, "Check", m_check);
    ui->menu_item(e, "Hint", m_hint);
    menu *h = ui->add_menu(s->win, "Help");
    ui->menu_item(h, "How to Play", m_help);
    ui->menu_item(h, "About Sudoku", m_about);

    for (int l = 0; l < LEVELS; l++) {
        char k[8];
        api->snprintf(k, sizeof k, "best%d", l);
        s->best[l] = api->prefs_get_int(k, 0);
    }
    s->rng = api->time_millis() | 1;
    if (load_game(s))
        redraw(s);
    else
        new_game(s, api->prefs_get_int("level", EASY) % LEVELS);
    ui->focus(s->view);
}

static void sd_frame(void *state) {
    sudoku_t *s = state;
    if (s->win && s->playing && !s->solved && (int)now_secs(s) != s->shown_secs)
        status(s); /* the status bar is a control: it redraws by itself */
}

static void sd_destroy(void *state) {
    sudoku_t *s = state;
    if (s->win && s->playing)
        save_game(s);
}

APP_META("Sudoku", sudoku_t, true, 0, sd_init, sd_frame, sd_destroy);
