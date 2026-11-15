/* Screen saver: three original effects drawn with the usual primitives.
 * Animation runs in fixed 30 ms steps, so speed is the same on a 386 and
 * a Pentium (slow machines just draw fewer of the steps). */
#include "ui/saver.h"

#include "config/runtime_config.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "lib/graphics.h"
#include "lib/time.h"
#include "ui/window.h"

const char *const SAVER_NAMES[SAVER_COUNT] = {"Starfield", "Mystic lines",
                                              "Rain", "Random"};

#define STEP_MS 30

static uint32_t s_rng = 0x2F6E2B1u;
static int rnd(int n) { /* 0..n-1, xorshift */
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return n > 0 ? (int)(s_rng % (uint32_t)n) : 0;
}

/* the rectangle the effect was set up for; a new one starts it over */
static int s_x, s_y, s_w, s_h, s_effect = -1;
static uint32_t s_t; /* time of the last step */

/* ── Starfield: flying forward through stars ────────────────────────────── */

#define MAX_STARS 160
static struct {
    int x, y, z;
} s_star[MAX_STARS];
static int s_nstars;

static void star_spawn(int i, bool anywhere) {
    s_star[i].x = rnd(2048) - 1024;
    s_star[i].y = rnd(2048) - 1024;
    s_star[i].z = anywhere ? 16 + rnd(240) : 256;
}

static bool star_project(int i, int z, int *sx, int *sy) {
    if (z < 1)
        return false;
    *sx = s_x + s_w / 2 + s_star[i].x * s_w / (z * 16);
    *sy = s_y + s_h / 2 + s_star[i].y * s_w / (z * 16);
    return *sx >= s_x && *sx < s_x + s_w && *sy >= s_y && *sy < s_y + s_h;
}

static void stars_init(void) {
    s_nstars = s_w * s_h / 1600;
    if (s_nstars < 16)
        s_nstars = 16;
    if (s_nstars > MAX_STARS)
        s_nstars = MAX_STARS;
    for (int i = 0; i < s_nstars; i++)
        star_spawn(i, true);
}

static void stars_step(void) {
    for (int i = 0; i < s_nstars; i++) {
        s_star[i].z -= 4;
        int sx, sy;
        if (!star_project(i, s_star[i].z, &sx, &sy))
            star_spawn(i, false);
    }
}

static void stars_draw(void) {
    for (int i = 0; i < s_nstars; i++) {
        int z = s_star[i].z, sx, sy, px, py;
        if (!star_project(i, z, &sx, &sy))
            continue;
        uint8_t col = z < 128 ? WHITE : z < 208 ? LIGHT_GRAY : DARK_GRAY;
        if (z < 64 && star_project(i, z + 10, &px, &py))
            draw_line(px, py, sx, sy, LIGHT_GRAY); /* close: a streak */
        if (z < 48)
            fill_rect(sx, sy, 2, 2, col);
        else
            draw_dot(sx, sy, col);
    }
}

/* ── Mystic lines: two bouncing quadrilaterals leaving echoes ───────────── */

#define POLYS 2
#define CORNERS 4
#define ECHOES 6
#define ECHO_GAP 3 /* steps between echoes */
typedef struct {
    int x[CORNERS], y[CORNERS]; /* 8.8 fixed point */
    int dx[CORNERS], dy[CORNERS];
    int ex[ECHOES][CORNERS], ey[ECHOES][CORNERS]; /* pixels */
    int head, filled;
    uint8_t col;
} poly_t;
static poly_t s_poly[POLYS];
static int s_poly_steps;
static const uint8_t POLY_COLS[] = {LIGHT_CYAN,  LIGHT_MAGENTA, YELLOW,
                                    LIGHT_GREEN, LIGHT_RED,     LIGHT_BLUE,
                                    WHITE};

static void lines_init(void) {
    for (int p = 0; p < POLYS; p++) {
        poly_t *q = &s_poly[p];
        for (int c = 0; c < CORNERS; c++) {
            q->x[c] = rnd(s_w) << 8;
            q->y[c] = rnd(s_h) << 8;
            int sp = s_w > 200 ? 384 : 160; /* slower in a small preview */
            q->dx[c] = (rnd(2) ? 1 : -1) * (sp / 2 + rnd(sp));
            q->dy[c] = (rnd(2) ? 1 : -1) * (sp / 2 + rnd(sp));
        }
        q->head = q->filled = 0;
        q->col = POLY_COLS[p * 3 % sizeof(POLY_COLS)];
    }
    s_poly_steps = 0;
}

static void bounce(int *v, int *d, int max) {
    *v += *d;
    if (*v < 0) {
        *v = -*v;
        *d = -*d;
    } else if (*v >= max << 8) {
        *v = (2 * (max << 8)) - *v - 1;
        *d = -*d;
    }
}

static void lines_step(void) {
    s_poly_steps++;
    for (int p = 0; p < POLYS; p++) {
        poly_t *q = &s_poly[p];
        for (int c = 0; c < CORNERS; c++) {
            bounce(&q->x[c], &q->dx[c], s_w);
            bounce(&q->y[c], &q->dy[c], s_h);
        }
        if (s_poly_steps % ECHO_GAP == 0) {
            q->head = (q->head + 1) % ECHOES;
            if (q->filled < ECHOES)
                q->filled++;
            for (int c = 0; c < CORNERS; c++) {
                q->ex[q->head][c] = q->x[c] >> 8;
                q->ey[q->head][c] = q->y[c] >> 8;
            }
        }
        if (s_poly_steps % 150 == 0) /* drift to the next colour */
            for (unsigned k = 0; k < sizeof(POLY_COLS); k++)
                if (POLY_COLS[k] == q->col) {
                    q->col = POLY_COLS[(k + 1) % sizeof(POLY_COLS)];
                    break;
                }
    }
}

static void lines_draw(void) {
    for (int p = 0; p < POLYS; p++) {
        poly_t *q = &s_poly[p];
        for (int e = 0; e < q->filled; e++) {
            int (*ex)[CORNERS] = q->ex, (*ey)[CORNERS] = q->ey;
            int i = (q->head - e + ECHOES) % ECHOES;
            uint8_t col = e < ECHOES - 2 ? q->col : DARK_GRAY; /* fade */
            for (int c = 0; c < CORNERS; c++) {
                int n = (c + 1) % CORNERS;
                draw_line(s_x + ex[i][c], s_y + ey[i][c], s_x + ex[i][n],
                          s_y + ey[i][n], col);
            }
        }
    }
}

/* ── Rain: drops falling into a dark pond, rings where they land ────────── */

#define MAX_DROPS 100
static struct {
    int x, y, ground, speed, len, ring; /* ring > 0: splashing */
} s_drop[MAX_DROPS];
static int s_ndrops;
/* a ring: 8 points of an ellipse twice as wide as tall, radius 8 */
static const int8_t RING_X[8] = {8, 6, 0, -6, -8, -6, 0, 6};
static const int8_t RING_Y[8] = {0, 2, 3, 2, 0, -2, -3, -2};

static void drop_spawn(int i, bool anywhere) {
    s_drop[i].x = rnd(s_w);
    s_drop[i].ground = s_h / 2 + rnd(s_h / 2); /* nearer = lower */
    s_drop[i].y = anywhere ? rnd(s_drop[i].ground) : -rnd(s_h / 2);
    int near = (s_drop[i].ground - s_h / 2) * 4 / (s_h / 2 + 1); /* 0..3 */
    s_drop[i].speed = 2 + near + (s_h > 150 ? near : 0);
    s_drop[i].len = 3 + near * 2;
    s_drop[i].ring = 0;
}

static void rain_init(void) {
    s_ndrops = s_w * s_h / 2500;
    if (s_ndrops < 8)
        s_ndrops = 8;
    if (s_ndrops > MAX_DROPS)
        s_ndrops = MAX_DROPS;
    for (int i = 0; i < s_ndrops; i++)
        drop_spawn(i, true);
}

static void rain_step(void) {
    for (int i = 0; i < s_ndrops; i++) {
        if (s_drop[i].ring) {
            if (++s_drop[i].ring > 8)
                drop_spawn(i, false);
            continue;
        }
        s_drop[i].y += s_drop[i].speed;
        if (s_drop[i].y >= s_drop[i].ground)
            s_drop[i].ring = 1;
    }
}

static void rain_draw(void) {
    for (int i = 0; i < s_ndrops; i++) {
        int x = s_x + s_drop[i].x, g = s_y + s_drop[i].ground;
        int near = (s_drop[i].len - 3) / 2; /* 0..3 */
        if (s_drop[i].ring) {
            int r = s_drop[i].ring; /* 1..8: grows, then fades */
            uint8_t col = r < 4 ? LIGHT_CYAN : r < 7 ? CYAN : BLUE;
            for (int k = 0; k < 8; k++)
                draw_dot(x + RING_X[k] * r * (near + 2) / 24,
                         g + RING_Y[k] * r * (near + 2) / 24, col);
            continue;
        }
        int y = s_y + s_drop[i].y;
        if (y < s_y)
            continue;
        uint8_t col = near >= 2 ? LIGHT_CYAN : near ? CYAN : BLUE;
        int top = y - s_drop[i].len < s_y ? s_y : y - s_drop[i].len;
        draw_line(x, top, x, y, col);
    }
}

/* ── driving it ────────────────────────────────────────────────────────── */

static int s_random_pick = SAVER_STARS;

void saver_paint(int x, int y, int w, int h, int effect) {
    if (effect == SAVER_RANDOM)
        effect = s_random_pick;
    if (effect < 0 || effect >= SAVER_RANDOM)
        effect = SAVER_STARS;
    uint32_t now = time_millis();
    if (x != s_x || y != s_y || w != s_w || h != s_h || effect != s_effect) {
        s_x = x;
        s_y = y;
        s_w = w > 1 ? w : 1;
        s_h = h > 1 ? h : 1;
        s_effect = effect;
        s_rng ^= now;
        if (effect == SAVER_STARS)
            stars_init();
        else if (effect == SAVER_LINES)
            lines_init();
        else
            rain_init();
        s_t = now;
    }
    int steps = 0;
    while (now - s_t >= STEP_MS && steps < 4) { /* at most 4 per frame */
        s_t += STEP_MS;
        steps++;
        if (effect == SAVER_STARS)
            stars_step();
        else if (effect == SAVER_LINES)
            lines_step();
        else
            rain_step();
    }
    if (now - s_t > STEP_MS * 4)
        s_t = now; /* far behind (slow machine): don't try to catch up */

    fill_rect(x, y, w, h, BLACK);
    if (effect == SAVER_STARS)
        stars_draw();
    else if (effect == SAVER_LINES)
        lines_draw();
    else
        rain_draw();
}

static bool s_active;
static uint32_t s_last_input, s_started;

bool saver_active(void) { return s_active; }

void saver_start(void) {
    s_active = true;
    s_started = time_millis();
    s_effect = -1; /* set up afresh */
    s_random_pick = rnd(SAVER_RANDOM);
}

static bool had_input(void) {
    return mouse.dx || mouse.dy || mouse.left_clicked || mouse.right_clicked ||
           kb.key_pressed;
}

bool saver_update(void) {
    uint32_t now = time_millis();
    if (had_input()) {
        s_last_input = now;
        if (s_active && now - s_started < 400) { /* the click that started
                                                    it (Test) settling */
            mouse.left_clicked = mouse.right_clicked = false;
            kb.key_pressed = false;
            return true;
        }
        if (s_active) { /* wake up; this input is only for that */
            s_active = false;
            mouse.left_clicked = mouse.right_clicked = false;
            kb.key_pressed = false;
            kb.last_char = 0;
            kb.last_scancode = 0;
            return false;
        }
    }
    if (!s_last_input)
        s_last_input = now;
    if (!s_active && g_cfg.saver_minutes &&
        now - s_last_input >= (uint32_t)g_cfg.saver_minutes * 60000u)
        saver_start();
    return s_active;
}

void saver_draw(void) {
    wm_invalidate_all(); /* the desktop is gone from the back buffer */
    gfx_reset_clip();
    saver_paint(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, g_cfg.saver_effect);
}
