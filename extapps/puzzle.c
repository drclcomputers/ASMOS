// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "extapps/shim.h"

#define N 4
#define TILE 22
#define BOARD (N * TILE)

typedef struct {
    window *win; /* must stay first */
    control *view, *status;
    uint8_t t[N * N]; /* 1..15, 0 = gap */
    int moves, best;
    bool solved, playing;
} puzzle_state_t;

app_descriptor puzzle_app;

static int gap_of(const puzzle_state_t *s) {
    for (int i = 0; i < N * N; i++)
        if (!s->t[i])
            return i;
    return N * N - 1;
}

static bool is_solved(const puzzle_state_t *s) {
    for (int i = 0; i < N * N - 1; i++)
        if (s->t[i] != i + 1)
            return false;
    return true;
}

static void show_status(puzzle_state_t *s) {
    char b[48];
    if (s->solved && s->playing)
        sprintf(b, "Solved in %d moves", s->moves);
    else if (s->best)
        sprintf(b, "Moves %d   Best %d", s->moves, s->best);
    else
        sprintf(b, "Moves %d", s->moves);
    ctl_set_text(s->status, b);
}

/* slide every tile between `at` and the gap one step toward the gap */
static bool slide_from(puzzle_state_t *s, int at) {
    int g = gap_of(s), gr = g / N, gc = g % N, r = at / N, c = at % N;
    if (at == g || (r != gr && c != gc))
        return false;
    int step = r == gr ? (c < gc ? -1 : 1) : (r < gr ? -N : N);
    while (g != at) {
        s->t[g] = s->t[g + step];
        g += step;
        s->moves++;
    }
    s->t[at] = 0;
    return true;
}

static void after_move(puzzle_state_t *s) {
    if (!s->solved && is_solved(s)) {
        s->solved = true;
        if (s->playing) {
            if (!s->best || s->moves < s->best) {
                s->best = s->moves;
                app_prefs_set_int("PUZZLE", "best", s->best);
            }
            speaker_beep_async(880, 60);
        }
    }
    show_status(s);
    wm_invalidate(s->win);
}

static void shuffle(puzzle_state_t *s) {
    for (int i = 0; i < N * N; i++)
        s->t[i] = (uint8_t)((i + 1) % (N * N));
    rand_seed(time_millis() ^ 0x5EED);
    /* random moves from the solved board: always solvable */
    int last = -1;
    for (int k = 0; k < 400 || is_solved(s); k++) {
        int g = gap_of(s), opt[4], n = 0;
        if (g % N > 0)
            opt[n++] = g - 1;
        if (g % N < N - 1)
            opt[n++] = g + 1;
        if (g >= N)
            opt[n++] = g - N;
        if (g < N * N - N)
            opt[n++] = g + N;
        int p;
        do
            p = opt[rand_range(0, n - 1)];
        while (p == last && n > 1);
        last = g;
        slide_from(s, p);
    }
    s->moves = 0;
    s->solved = false;
    s->playing = true;
    after_move(s);
}

static void board_origin(const control *c, int x, int y, int w, int h, int *bx,
                         int *by) {
    (void)c;
    *bx = x + (w - BOARD) / 2;
    *by = y + (h - BOARD) / 2;
}

static void paint(control *c, int x, int y, int w, int h) {
    puzzle_state_t *s = c->user;
    int bx, by;
    fill_rect(x, y, w, h, LIGHT_GRAY);
    board_origin(c, x, y, w, h, &bx, &by);
    for (int i = 0; i < N * N; i++) {
        if (!s->t[i])
            continue;
        int tx = bx + (i % N) * TILE, ty = by + (i / N) * TILE;
        uint8_t face = s->solved ? WHITE : DARK_GRAY;
        fill_rect(tx + 1, ty + 1, TILE - 2, TILE - 2, face);
        draw_rect(tx, ty, TILE, TILE, BLACK);
        draw_line(tx + 1, ty + 1, tx + TILE - 3, ty + 1, WHITE);
        draw_line(tx + 1, ty + 1, tx + 1, ty + TILE - 3, WHITE);
        draw_line(tx + 1, ty + TILE - 2, tx + TILE - 2, ty + TILE - 2,
                  DARK_GRAY);
        draw_line(tx + TILE - 2, ty + 1, tx + TILE - 2, ty + TILE - 2,
                  DARK_GRAY);
        char n[3];
        sprintf(n, "%d", s->t[i]);
        int f = UI_FONT_BOLD;
        draw_string(tx + (TILE - text_width(n, f)) / 2,
                    ty + (TILE - font_height(f)) / 2, n,
                    s->solved ? BLACK : WHITE, f);
    }
}

static void on_view(control *c, int ev) {
    puzzle_state_t *s = c->user;
    int at = -1, g = gap_of(s);
    if (ev == EV_MOUSEDOWN) {
        int bx, by;
        board_origin(c, 0, 0, c->aw, c->ah, &bx, &by);
        int col = (c->mx - bx) / TILE, row = (c->my - by) / TILE;
        if (c->mx >= bx && c->my >= by && col < N && row < N)
            at = row * N + col;
    } else if (ev == EV_KEY) {
        /* the arrow moves a tile in that direction, into the gap */
        switch (kb.last_scancode) {
        case LEFT_ARROW:
            if (g % N < N - 1)
                at = g + 1;
            break;
        case RIGHT_ARROW:
            if (g % N > 0)
                at = g - 1;
            break;
        case UP_ARROW:
            if (g < N * N - N)
                at = g + N;
            break;
        case DOWN_ARROW:
            if (g >= N)
                at = g - N;
            break;
        }
    }
    if (at < 0 || s->solved)
        return;
    if (slide_from(s, at))
        after_move(s);
}

static void menu_new(void) {
    puzzle_state_t *s = ui_active_state();
    if (s)
        shuffle(s);
}

static void menu_close(void) { os_quit_app_by_desc(&puzzle_app); }

static void menu_about(void) {
    dlg_message(NULL, "About Puzzle",
                "Puzzle v1.2\nPut the tiles in order, 1 to 15.\n"
                "Click a tile or use the arrow keys.");
}

static void puzzle_init(void *state) {
    puzzle_state_t *s = state;
    int cw = BOARD + 8, ch = BOARD + 8 + 16;
    s->win = app_window(s, "Puzzle", 40, 40, cw + 2, ch + 17, DARK_GRAY, 0);
    if (!s->win)
        return;
    s->win->lazy_custom = true; /* the board only changes in its handler */
    s->view = ctl_custom(s->win, 0, 0, cw, BOARD + 8, paint, on_view,
                         CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);
    s->status = ctl_status(s->win, "");
    s->best = app_prefs_get_int("PUZZLE", "best", 0);

    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "New Game", menu_new);
    menu_add_separator(f);
    menu_add_item(f, "Close", menu_close);
    menu_add_item(f, "About Puzzle", menu_about);
    shuffle(s);
    ctl_focus(s->view);
}

APP_META("Puzzle", puzzle_state_t, true, 0, puzzle_init, NULL, NULL);
