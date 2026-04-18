#include "os/api.h"

#define MUSIC_DEFAULT_X 10
#define MUSIC_DEFAULT_Y 10
#define MUSIC_DEFAULT_W 300
#define MUSIC_DEFAULT_H 180

#define ROWS 12
#define COLS 16

#define CELL_W 13
#define CELL_H 9

#define GRID_X 38
#define GRID_Y 14
#define GRID_W (COLS * CELL_W)
#define GRID_H (ROWS * CELL_H)

#define CTRL_Y (GRID_Y + GRID_H + 6)
#define BTN_H 10
#define BTN_W 24

#define STATUS_Y (CTRL_Y + BTN_H + 4)

static const uint32_t NOTE_FREQ[ROWS] = {
    880, // A5
    784, // G5
    698, // F5
    659, // E5
    587, // D5
    523, // C5
    494, // B4
    440, // A4
    392, // G4
    349, // F4
    330, // E4
    294, // D4
};

static const char *NOTE_NAME[ROWS] = {
    "A5", "G5", "F5", "E5", "D5", "C5", "B4", "A4", "G4", "F4", "E4", "D4",
};

#define NUM_BPMS 21
static const uint16_t BPM_VALUES[NUM_BPMS] = {
    60,  65,  70,  75,  80,  85,  90,  95,  100, 105, 110,
    115, 120, 125, 130, 135, 140, 150, 160, 170, 180};

typedef struct {
    window *win;

    bool grid[ROWS][COLS];

    bool playing;
    int cur_col;
    uint32_t tick_accum;
    uint32_t ticks_per_step;

    int bpm_idx;

    bool note_on;
    uint32_t note_off_tick;

    char status[48];
    uint32_t status_timer;
} asmusic_state_t;

app_descriptor asmusic_app;

static void asmusic_set_status(asmusic_state_t *s, const char *msg) {
    strncpy(s->status, msg, 47);
    s->status[47] = '\0';
    s->status_timer = 180;
}

// Convert BPM to pit_ticks per step.
// One beat = 60/BPM seconds.  One step = one beat / 4 (16th-note grid).
// pit_ticks runs at ~100 ticks/sec (PIT_DIVISOR=11931 → ~100 Hz).
static uint32_t bpm_to_ticks(uint16_t bpm) {
    uint32_t ticks_per_beat = (uint32_t)(6000 / bpm);
    if (ticks_per_beat < 1)
        ticks_per_beat = 1;
    uint32_t tps = ticks_per_beat / 4;
    if (tps < 1)
        tps = 1;
    return tps;
}

static int win_cx(const asmusic_state_t *s) { return s->win->x + 1; }
static int win_cy(const asmusic_state_t *s) {
    return s->win->y + MENUBAR_H + 16;
}

static bool grid_hit(const asmusic_state_t *s, int mx, int my, int *row,
                     int *col) {
    int ox = win_cx(s) + GRID_X;
    int oy = win_cy(s) + GRID_Y;
    int rx = mx - ox;
    int ry = my - oy;
    if (rx < 0 || ry < 0 || rx >= GRID_W || ry >= GRID_H)
        return false;
    *col = rx / CELL_W;
    *row = ry / CELL_H;
    return true;
}

static bool btn_hit(int mx, int my, int bx, int by, int bw, int bh) {
    return mx >= bx && mx < bx + bw && my >= by && my < by + bh;
}

static void asmusic_draw(window *win, void *ud) {
    asmusic_state_t *s = (asmusic_state_t *)ud;
    if (!s)
        return;

    int cx = win_cx(s);
    int cy = win_cy(s);
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(cx, cy, ww, wh, BLACK);

    for (int r = 0; r < ROWS; r++) {
        int ly = cy + GRID_Y + r * CELL_H + 1;
        uint8_t col = (NOTE_NAME[r][0] == 'C') ? CYAN : DARK_GRAY;
        draw_string(cx + 2, ly, (char *)NOTE_NAME[r], col, 2);
    }

    int gx = cx + GRID_X;
    int gy = cy + GRID_Y;

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int bx = gx + c * CELL_W;
            int by = gy + r * CELL_H;

            bool active = s->grid[r][c];
            bool playhead = (s->playing && c == s->cur_col);
            bool beat_col = (c % 4 == 0);

            uint8_t bg;
            if (playhead && active) {
                bg = WHITE;
            } else if (playhead) {
                bg = DARK_GRAY;
            } else if (active) {
                if (r < 6)
                    bg = LIGHT_BLUE;
                else
                    bg = LIGHT_GREEN;
            } else if (beat_col) {
                bg = 1;
            } else {
                bg = BLACK;
            }

            fill_rect(bx + 1, by + 1, CELL_W - 2, CELL_H - 2, bg);

            uint8_t border = beat_col ? DARK_GRAY : 8;
            draw_rect(bx, by, CELL_W, CELL_H, border);
        }
    }

    if (s->playing) {
        int px = gx + s->cur_col * CELL_W;
        draw_line(px, gy - 2, px + CELL_W - 1, gy - 2, CYAN);
    }

    int bx = cx + 2;
    int by = cy + CTRL_Y;

    {
        uint8_t pbg = s->playing ? RED : LIGHT_GREEN;
        const char *lbl = s->playing ? "STOP" : "PLAY";
        fill_rect(bx, by, BTN_W + 8, BTN_H, pbg);
        draw_rect(bx, by, BTN_W + 8, BTN_H, WHITE);
        draw_string(bx + 3, by + 2, (char *)lbl, WHITE, 2);
        bx += BTN_W + 12;
    }

    {
        fill_rect(bx, by, BTN_W, BTN_H, DARK_GRAY);
        draw_rect(bx, by, BTN_W, BTN_H, WHITE);
        draw_string(bx + 2, by + 2, "CLR", WHITE, 2);
        bx += BTN_W + 4;
    }

    {
        draw_string(bx, by + 2, "BPM:", LIGHT_GRAY, 2);
        bx += 22;

        fill_rect(bx, by, 10, BTN_H, DARK_GRAY);
        draw_rect(bx, by, 10, BTN_H, WHITE);
        draw_string(bx + 2, by + 2, "<", WHITE, 2);
        bx += 12;

        char bpm_str[8];
        sprintf(bpm_str, "%d", BPM_VALUES[s->bpm_idx]);
        fill_rect(bx, by, 22, BTN_H, BLACK);
        draw_rect(bx, by, 22, BTN_H, DARK_GRAY);
        draw_string(bx + 2, by + 2, bpm_str, CYAN, 2);
        bx += 24;

        fill_rect(bx, by, 10, BTN_H, DARK_GRAY);
        draw_rect(bx, by, 10, BTN_H, WHITE);
        draw_string(bx + 2, by + 2, ">", WHITE, 2);
        bx += 14;
    }

    if (s->status_timer > 0) {
        draw_string(cx + 2, cy + STATUS_Y, s->status, LIGHT_YELLOW, 2);
    } else {
        draw_string(cx + 2, cy + STATUS_Y,
                    s->playing ? "playing..." : "click cells to draw a melody",
                    DARK_GRAY, 2);
    }
}

static void asmusic_tick_playback(asmusic_state_t *s) {
    extern volatile uint32_t pit_ticks;

    if (s->note_on && pit_ticks >= s->note_off_tick) {
        speaker_tone_stop();
        s->note_on = false;
    }

    if (!s->playing)
        return;

    if (s->note_on && pit_ticks >= s->note_off_tick) {
        speaker_tone_stop();
        s->note_on = false;
    }

    s->tick_accum++;
    if (s->tick_accum < s->ticks_per_step)
        return;
    s->tick_accum = 0;

    s->cur_col = (s->cur_col + 1) % COLS;

    for (int r = 0; r < ROWS; r++) {
        if (s->grid[r][s->cur_col]) {
            speaker_tone_start(NOTE_FREQ[r]);
            s->note_on = true;
            uint32_t dur = (s->ticks_per_step * 6) / 10;
            if (dur < 1)
                dur = 1;
            s->note_off_tick = pit_ticks + dur;
            break;
        }
    }
}

static asmusic_state_t *active_asmusic(void) {
    window *fw = wm_focused_window();
    if (!fw)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &asmusic_app)
            continue;
        asmusic_state_t *s = (asmusic_state_t *)a->state;
        if (s->win == fw)
            return s;
    }
    return NULL;
}

static void menu_play_stop(void) {
    asmusic_state_t *s = active_asmusic();
    if (!s)
        return;
    s->playing = !s->playing;
    s->cur_col = -1;
    s->tick_accum = 0;
    if (!s->playing) {
        speaker_tone_stop();
        s->note_on = false;
    }
}

static void menu_clear(void) {
    asmusic_state_t *s = active_asmusic();
    if (!s)
        return;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            s->grid[r][c] = false;
    asmusic_set_status(s, "Grid cleared.");
}

static void menu_bpm_down(void) {
    asmusic_state_t *s = active_asmusic();
    if (!s)
        return;
    if (s->bpm_idx > 0)
        s->bpm_idx--;
    s->ticks_per_step = bpm_to_ticks(BPM_VALUES[s->bpm_idx]);
}

static void menu_bpm_up(void) {
    asmusic_state_t *s = active_asmusic();
    if (!s)
        return;
    if (s->bpm_idx < NUM_BPMS - 1)
        s->bpm_idx++;
    s->ticks_per_step = bpm_to_ticks(BPM_VALUES[s->bpm_idx]);
}

static void menu_close_asmusic(void) {
    asmusic_state_t *s = active_asmusic();
    if (!s)
        return;
    os_quit_app_by_desc(&asmusic_app);
}

static void on_about_asmusic(void) {
    modal_show(MODAL_INFO, "About ASMusic",
               "ASMusic v1.0\nASMOS Step Sequencer\n16 steps, 12 notes", NULL,
               NULL);
}

static bool asmusic_close(window *w) {
    (void)w;
    speaker_tone_stop();
    os_quit_app_by_desc(&asmusic_app);
    return true;
}

static void asmusic_init(void *state) {
    asmusic_state_t *s = (asmusic_state_t *)state;

    const window_spec_t spec = {
        .x = MUSIC_DEFAULT_X,
        .y = MUSIC_DEFAULT_Y,
        .w = MUSIC_DEFAULT_W,
        .h = MUSIC_DEFAULT_H,
        .min_w = MUSIC_DEFAULT_W,
        .min_h = MUSIC_DEFAULT_H,
        .resizable = false,
        .title = "ASMusic",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = BLACK,
        .visible = true,
        .on_close = asmusic_close,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;

    s->win->on_draw = asmusic_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Play / Stop", menu_play_stop);
    menu_add_item(file_menu, "Clear", menu_clear);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "BPM -", menu_bpm_down);
    menu_add_item(file_menu, "BPM +", menu_bpm_up);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", menu_close_asmusic);
    menu_add_item(file_menu, "About", on_about_asmusic);

    s->bpm_idx = 9;
    s->ticks_per_step = bpm_to_ticks(BPM_VALUES[s->bpm_idx]);
    s->playing = false;
    s->cur_col = 0;
    s->tick_accum = 0;
    s->note_on = false;

    // Linkin Park "In the End" inspired riff - 16 step pattern
    int demo_e5 = 3;  // E5
    int demo_g5 = 1;  // G5
    int demo_a5 = 0;  // A5
    int demo_b4 = 6;  // B4
    int demo_a4 = 7;  // A4
    int demo_e4 = 10; // E4

    s->grid[demo_e5][0] = true;  // E5
    s->grid[demo_g5][1] = true;  // G5
    s->grid[demo_b4][2] = true;  // B4
    s->grid[demo_a4][3] = true;  // A4
    s->grid[demo_e5][4] = true;  // E5
    s->grid[demo_g5][5] = true;  // G5
    s->grid[demo_a5][6] = true;  // A5
    s->grid[demo_g5][7] = true;  // G5
    s->grid[demo_e5][8] = true;  // E5
    s->grid[demo_b4][9] = true;  // B4
    s->grid[demo_e4][10] = true; // E4
    s->grid[demo_g5][11] = true; // G5
    s->grid[demo_a4][12] = true; // A4
    s->grid[demo_g5][13] = true; // G5
    s->grid[demo_b4][14] = true; // B4
    s->grid[demo_e5][15] = true; // E5
}

static void asmusic_on_frame(void *state) {
    asmusic_state_t *s = (asmusic_state_t *)state;
    if (!s->win || !s->win->visible)
        return;

    asmusic_tick_playback(s);

    if (s->win->minimized)
        return;

    if (s->status_timer > 0)
        s->status_timer--;

    if (mouse.left_clicked) {
        int cx = win_cx(s);
        int cy = win_cy(s);

        int row, col;
        if (grid_hit(s, mouse.x, mouse.y, &row, &col)) {
            s->grid[row][col] = !s->grid[row][col];
            if (s->grid[row][col]) {
                speaker_tone_start(NOTE_FREQ[row]);
                s->note_on = true;
                extern volatile uint32_t pit_ticks;
                s->note_off_tick = pit_ticks + 200;
            }
            asmusic_draw(s->win, s);
            return;
        }

        int bx = cx + 2;
        int by = cy + CTRL_Y;

        if (btn_hit(mouse.x, mouse.y, bx, by, BTN_W + 8, BTN_H)) {
            menu_play_stop();
            asmusic_draw(s->win, s);
            return;
        }
        bx += BTN_W + 12;

        if (btn_hit(mouse.x, mouse.y, bx, by, BTN_W, BTN_H)) {
            menu_clear();
            asmusic_draw(s->win, s);
            return;
        }
        bx += BTN_W + 4;

        bx += 22;

        if (btn_hit(mouse.x, mouse.y, bx, by, 10, BTN_H)) {
            menu_bpm_down();
            asmusic_draw(s->win, s);
            return;
        }
        bx += 12 + 24;

        if (btn_hit(mouse.x, mouse.y, bx, by, 10, BTN_H)) {
            menu_bpm_up();
            asmusic_draw(s->win, s);
            return;
        }
    }

    if (kb.key_pressed) {
        if (kb.last_char == ' ') {
            menu_play_stop();
        } else if (kb.last_char == 'c' || kb.last_char == 'C') {
            menu_clear();
        } else if (kb.last_scancode == LEFT_ARROW) {
            menu_bpm_down();
        } else if (kb.last_scancode == RIGHT_ARROW) {
            menu_bpm_up();
        }
    }

    asmusic_draw(s->win, s);
}

static void asmusic_destroy(void *state) {
    asmusic_state_t *s = (asmusic_state_t *)state;
    speaker_tone_stop();
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor asmusic_app = {
    .name = "ASMusic",
    .state_size = sizeof(asmusic_state_t),
    .init = asmusic_init,
    .on_frame = asmusic_on_frame,
    .destroy = asmusic_destroy,
};
