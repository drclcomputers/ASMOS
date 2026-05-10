#include "drivers/opl2.h"
#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "os/api.h"

// ─── Window geometry
#define MIDI_DEFAULT_X 20
#define MIDI_DEFAULT_Y 20
#define MIDI_DEFAULT_W 260
#define MIDI_DEFAULT_H 190

// ─── Layout constants
#define PAD 6

// Path input bar
#define PATH_Y 14
#define PATH_H 12
#define PATH_X PAD
#define PATH_W (MIDI_DEFAULT_W - PAD * 2 - 28)
#define LOAD_BTN_X (PATH_X + PATH_W + 4)
#define LOAD_BTN_W 24

// Visualizer / now-playing panel
#define VIZ_Y (PATH_Y + PATH_H + 6)
#define VIZ_H 52
#define VIZ_W (MIDI_DEFAULT_W - PAD * 2)

// Transport bar
#define TRANS_Y (VIZ_Y + VIZ_H + 6)
#define TRANS_H 14
#define BTN_SZ 20 // square buttons
#define LOOP_BTN_W 30

// Progress bar
#define PROG_Y (TRANS_Y + TRANS_H + 6)
#define PROG_H 6
#define PROG_W (MIDI_DEFAULT_W - PAD * 2)

// Volume
#define VOL_Y (PROG_Y + PROG_H + 8)
#define VOL_H 10
#define VOL_LABEL_W 24
#define VOL_TRACK_X (PAD + VOL_LABEL_W + 4)
#define VOL_TRACK_W (MIDI_DEFAULT_W - PAD * 2 - VOL_LABEL_W - 24)
#define VOL_VAL_X (VOL_TRACK_X + VOL_TRACK_W + 4)

// Status
#define STATUS_Y (VOL_Y + VOL_H + 8)

// ─── VU meter bars
#define VU_BARS 24
#define VU_BAR_W ((VIZ_W - 2) / VU_BARS)
#define VU_BAR_MAXH (VIZ_H - 2)

// ─── Misc
#define MAX_PATH_LEN 63
#define MAX_MIDI_SIZE (64 * 1024) // 64 KB — plenty for standard MIDIs

typedef enum {
    MIDI_STATE_IDLE,
    MIDI_STATE_LOADED,
    MIDI_STATE_PLAYING,
    MIDI_STATE_PAUSED,
} midi_ui_state_t;

typedef struct {
    window *win;

    // File path input
    char path_buf[MAX_PATH_LEN + 1];
    int path_len;
    bool path_focused;

    // Loaded MIDI
    uint8_t *midi_buf;
    uint32_t midi_size;
    char filename[32]; // basename for display

    // Playback state
    midi_ui_state_t state;
    bool looping;
    uint8_t volume; // 0-100

    // Visualizer: per-bar peak heights (smoothly decaying)
    uint8_t vu_peaks[VU_BARS];
    uint8_t vu_decay[VU_BARS];
    uint32_t vu_frame;

    // Progress tracking
    uint32_t play_start_pit; // pit_ticks when play began
    uint32_t total_pit_est;  // estimated total length in pit_ticks

    // Status message
    char status[64];
    uint32_t status_timer;

    // Volume drag
    bool vol_dragging;
} midiplayer_state_t;

app_descriptor midiplayer_app;

// ─── Helpers

static void mp_set_status(midiplayer_state_t *s, const char *msg) {
    strncpy(s->status, msg, 63);
    s->status[63] = '\0';
    s->status_timer = 240;
}

static int win_cx(const midiplayer_state_t *s) { return s->win->x + 1; }
static int win_cy(const midiplayer_state_t *s) {
    return s->win->y + MENUBAR_H + 16;
}

static bool btn_hit(int mx, int my, int bx, int by, int bw, int bh) {
    return mx >= bx && mx < bx + bw && my >= by && my < by + bh;
}

// ─── VU meter update ─────────────────────────────────────────────────────────
// Driven by a simple pseudo-random pattern that reacts to OPL2 activity.
// In a real implementation you'd sample OPL2 register state; here we fake it
// nicely so it looks musically alive.
static void mp_update_vu(midiplayer_state_t *s) {
    if (s->state != MIDI_STATE_PLAYING) {
        // Decay all bars to zero
        for (int i = 0; i < VU_BARS; i++) {
            if (s->vu_peaks[i] > 0)
                s->vu_peaks[i] = s->vu_peaks[i] > 3 ? s->vu_peaks[i] - 3 : 0;
        }
        return;
    }

    s->vu_frame++;

    // Simple pseudo-random noise that looks like a spectrum
    // In practice, hook OPL2 channel note_on events to feed real data here.
    uint32_t t = s->vu_frame;
    for (int i = 0; i < VU_BARS; i++) {
        // Each bar has its own oscillation frequency based on index
        uint32_t phase = t * (uint32_t)(i + 3);
        uint8_t wave = (uint8_t)((phase ^ (phase >> 4) ^ (phase >> 7)) & 0xFF);
        // Shape into a rough spectrum curve (higher bars in low-mid range)
        uint8_t shape;
        if (i < 4)
            shape = 60 + (uint8_t)(i * 8);
        else if (i < 10)
            shape = 85 + (uint8_t)((i - 4) * 2);
        else if (i < 18)
            shape = 75 - (uint8_t)((i - 10) * 5);
        else
            shape = 30 - (uint8_t)((i - 18) * 4);

        uint8_t target = (uint8_t)((wave * shape) >> 8);
        // Scale to bar height
        target = (uint8_t)((target * VU_BAR_MAXH) >> 8);
        // Apply volume scaling
        target = (uint8_t)((target * (uint32_t)s->volume) / 100);

        if (target > s->vu_peaks[i]) {
            s->vu_peaks[i] = target;
        } else {
            // Smooth decay
            uint8_t decay =
                (uint8_t)(s->vu_decay[i] > 0 ? s->vu_decay[i]-- : 0);
            (void)decay;
            if (s->vu_peaks[i] > 2)
                s->vu_peaks[i] -= 2;
            else
                s->vu_peaks[i] = 0;
        }
    }
}

// ─── Draw ────────────────────────────────────────────────────────────────────

static void midiplayer_draw(window *win, void *ud) {
    midiplayer_state_t *s = (midiplayer_state_t *)ud;
    if (!s)
        return;

    int cx = win_cx(s);
    int cy = win_cy(s);
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(cx, cy, ww, wh, BLACK);

    // ── Path input ────────────────────────────────────────────────────────
    int py = cy + PATH_Y;
    uint8_t path_border = s->path_focused ? CYAN : DARK_GRAY;
    fill_rect(cx + PATH_X, py, PATH_W, PATH_H, 1);
    draw_rect(cx + PATH_X, py, PATH_W, PATH_H, path_border);

    char display_path[MAX_PATH_LEN + 1];
    // Show last N chars that fit (each char ~5px wide in font mode 2)
    int max_vis = (PATH_W - 4) / 5;
    int start = s->path_len > max_vis ? s->path_len - max_vis : 0;
    strncpy(display_path, s->path_buf + start, max_vis);
    display_path[max_vis] = '\0';
    draw_string(cx + PATH_X + 2, py + 3, display_path, LIGHT_GRAY, 2);

    // Cursor
    if (s->path_focused) {
        int vis_len = s->path_len - start;
        int cursor_x = cx + PATH_X + 2 + vis_len * 5;
        if (cursor_x < cx + PATH_X + PATH_W - 2)
            draw_string(cursor_x, py + 3, "|", CYAN, 2);
    }

    // LOAD button
    int lbx = cx + LOAD_BTN_X;
    fill_rect(lbx, py, LOAD_BTN_W, PATH_H, DARK_GRAY);
    draw_rect(lbx, py, LOAD_BTN_W, PATH_H, LIGHT_GRAY);
    draw_string(lbx + 3, py + 3, "GO", WHITE, 2);

    // ── VU Meter / now-playing panel ─────────────────────────────────────
    int vx = cx + PAD;
    int vy = cy + VIZ_Y;
    fill_rect(vx, vy, VIZ_W, VIZ_H, 1);
    draw_rect(vx, vy, VIZ_W, VIZ_H, DARK_GRAY);

    if (s->state == MIDI_STATE_IDLE) {
        draw_string(vx + 8, vy + VIZ_H / 2 - 3, "No file loaded", DARK_GRAY, 2);
    } else {
        // Filename
        draw_string(vx + 3, vy + 3, s->filename, CYAN, 2);

        // VU bars
        int bar_bottom = vy + VIZ_H - 2;
        for (int i = 0; i < VU_BARS; i++) {
            int bx = vx + 1 + i * VU_BAR_W;
            int bh = s->vu_peaks[i];
            if (bh < 1)
                bh = 1;
            // Color: green → yellow → red based on height
            uint8_t col;
            if (bh < VU_BAR_MAXH * 5 / 10)
                col = LIGHT_GREEN;
            else if (bh < VU_BAR_MAXH * 8 / 10)
                col = YELLOW;
            else
                col = LIGHT_RED;
            fill_rect(bx, bar_bottom - bh, VU_BAR_W - 1, bh, col);
        }
    }

    // ── Transport buttons ─────────────────────────────────────────────────
    int tx = cx + PAD;
    int ty = cy + TRANS_Y;

    // |<< (rewind/restart)
    {
        bool active = (s->state != MIDI_STATE_IDLE);
        fill_rect(tx, ty, BTN_SZ, BTN_SZ, active ? DARK_GRAY : 1);
        draw_rect(tx, ty, BTN_SZ, BTN_SZ, active ? LIGHT_GRAY : DARK_GRAY);
        draw_string(tx + 3, ty + 4, "|<", active ? WHITE : DARK_GRAY, 2);
        tx += BTN_SZ + 3;
    }

    // PLAY / PAUSE
    {
        bool is_playing = (s->state == MIDI_STATE_PLAYING);
        bool active = (s->state != MIDI_STATE_IDLE);
        uint8_t pbg = is_playing ? RED : (active ? LIGHT_GREEN : 1);
        fill_rect(tx, ty, BTN_SZ, BTN_SZ, pbg);
        draw_rect(tx, ty, BTN_SZ, BTN_SZ, active ? WHITE : DARK_GRAY);
        const char *plbl =
            is_playing ? "||" : (s->state == MIDI_STATE_PAUSED ? ">>" : " >");
        draw_string(tx + 3, ty + 4, (char *)plbl, WHITE, 2);
        tx += BTN_SZ + 3;
    }

    // STOP
    {
        bool active =
            (s->state == MIDI_STATE_PLAYING || s->state == MIDI_STATE_PAUSED);
        fill_rect(tx, ty, BTN_SZ, BTN_SZ, active ? DARK_GRAY : 1);
        draw_rect(tx, ty, BTN_SZ, BTN_SZ, active ? LIGHT_GRAY : DARK_GRAY);
        draw_string(tx + 5, ty + 4, "[]", active ? WHITE : DARK_GRAY, 2);
        tx += BTN_SZ + 3;
    }

    // LOOP toggle
    {
        uint8_t lbg = s->looping ? LIGHT_BLUE : DARK_GRAY;
        fill_rect(tx, ty, LOOP_BTN_W, BTN_SZ, lbg);
        draw_rect(tx, ty, LOOP_BTN_W, BTN_SZ, s->looping ? WHITE : LIGHT_GRAY);
        draw_string(tx + 4, ty + 4, "LOOP", s->looping ? WHITE : LIGHT_GRAY, 2);
        tx += LOOP_BTN_W + 6;
    }

    // State label on the right
    {
        const char *state_lbl;
        uint8_t state_col;
        switch (s->state) {
        case MIDI_STATE_IDLE:
            state_lbl = "IDLE";
            state_col = DARK_GRAY;
            break;
        case MIDI_STATE_LOADED:
            state_lbl = "READY";
            state_col = LIGHT_GRAY;
            break;
        case MIDI_STATE_PLAYING:
            state_lbl = "PLAYING";
            state_col = LIGHT_GREEN;
            break;
        case MIDI_STATE_PAUSED:
            state_lbl = "PAUSED";
            state_col = YELLOW;
            break;
        default:
            state_lbl = "";
            state_col = DARK_GRAY;
            break;
        }
        int sw = (int)strlen(state_lbl) * 5;
        draw_string(cx + PAD + VIZ_W - sw, cy + TRANS_Y + 5, (char *)state_lbl,
                    state_col, 2);
    }

    // ── Progress bar ──────────────────────────────────────────────────────
    int prog_x = cx + PAD;
    int prog_y = cy + PROG_Y;

    fill_rect(prog_x, prog_y, PROG_W, PROG_H, 1);
    draw_rect(prog_x, prog_y, PROG_W, PROG_H, DARK_GRAY);

    if (s->state == MIDI_STATE_PLAYING || s->state == MIDI_STATE_PAUSED) {
        extern volatile uint32_t pit_ticks;
        uint32_t elapsed = pit_ticks - s->play_start_pit;
        uint32_t total = s->total_pit_est > 0 ? s->total_pit_est : 1;
        if (elapsed > total)
            elapsed = total;
        int fill_w = (int)((uint32_t)(PROG_W - 2) * elapsed / total);
        if (fill_w > 0)
            fill_rect(prog_x + 1, prog_y + 1, fill_w, PROG_H - 2, CYAN);
    }

    // ── Volume ────────────────────────────────────────────────────────────
    int vol_y = cy + VOL_Y;
    draw_string(cx + PAD, vol_y + 1, "VOL", LIGHT_GRAY, 2);

    int track_x = cx + VOL_TRACK_X;
    fill_rect(track_x, vol_y + 3, VOL_TRACK_W, VOL_H - 6, 1);
    draw_rect(track_x, vol_y + 3, VOL_TRACK_W, VOL_H - 6, DARK_GRAY);
    int knob_x = track_x + (int)((uint32_t)VOL_TRACK_W * s->volume / 100);
    fill_rect(knob_x - 3, vol_y, 6, VOL_H, LIGHT_GRAY);
    draw_rect(knob_x - 3, vol_y, 6, VOL_H, WHITE);

    char vol_str[5];
    sprintf(vol_str, "%d%%", (int)s->volume);
    draw_string(cx + VOL_VAL_X, vol_y + 1, vol_str, CYAN, 2);

    // ── Status ────────────────────────────────────────────────────────────
    if (s->status_timer > 0)
        draw_string(cx + PAD, cy + STATUS_Y, s->status, YELLOW, 2);
    else
        draw_string(cx + PAD, cy + STATUS_Y,
                    s->state == MIDI_STATE_IDLE ? "Type a path and press GO"
                                                : "",
                    DARK_GRAY, 2);
}

// ─── File loading
// ─────────────────────────────────────────────────────────────

static void mp_load_file(midiplayer_state_t *s) {
    if (s->path_len == 0) {
        mp_set_status(s, "No path entered.");
        return;
    }

    // Stop current playback
    if (s->state == MIDI_STATE_PLAYING || s->state == MIDI_STATE_PAUSED) {
        midi_player_stop();
        s->state = MIDI_STATE_IDLE;
    }

    // Free old buffer
    if (s->midi_buf) {
        kfree(s->midi_buf);
        s->midi_buf = NULL;
        s->midi_size = 0;
    }

    fat_file_t f;
    if (!fs_open(s->path_buf, &f)) {
        mp_set_status(s, "File not found.");
        return;
    }

    uint32_t fsize = f.entry.file_size;
    if (fsize == 0 || fsize > MAX_MIDI_SIZE) {
        fs_close(&f);
        mp_set_status(s, fsize == 0 ? "Empty file."
                                    : "File too large (max 64KB).");
        return;
    }

    uint8_t *buf = (uint8_t *)kmalloc(fsize);
    if (!buf) {
        fs_close(&f);
        mp_set_status(s, "Out of memory.");
        return;
    }

    int got = fs_read(&f, buf, (int)fsize);
    fs_close(&f);

    if (got < 4 || buf[0] != 'M' || buf[1] != 'T' || buf[2] != 'h' ||
        buf[3] != 'd') {
        kfree(buf);
        mp_set_status(s, "Not a MIDI file.");
        return;
    }

    if (!midi_player_load(buf, (uint32_t)got)) {
        kfree(buf);
        mp_set_status(s, "MIDI parse failed.");
        return;
    }

    s->midi_buf = buf;
    s->midi_size = (uint32_t)got;
    s->state = MIDI_STATE_LOADED;

    // Extract basename for display
    const char *slash = NULL;
    for (int i = 0; i < s->path_len; i++)
        if (s->path_buf[i] == '/' || s->path_buf[i] == '\\')
            slash = s->path_buf + i;
    const char *base = slash ? slash + 1 : s->path_buf;
    strncpy(s->filename, base, 31);
    s->filename[31] = '\0';

    // Rough duration estimate: MIDI duration is hard to know without parsing
    // the entire event stream.  We estimate from file size as a heuristic.
    // A typical 30-second MIDI ≈ 5KB, so bytes/170 ≈ seconds.
    // pit_ticks runs at ~100 Hz.
    s->total_pit_est = (fsize / 170) * 100;
    if (s->total_pit_est < 600)
        s->total_pit_est = 600;
    if (s->total_pit_est > 60000)
        s->total_pit_est = 60000;

    // Reset VU
    for (int i = 0; i < VU_BARS; i++) {
        s->vu_peaks[i] = 0;
        s->vu_decay[i] = 0;
    }

    char msg[64];
    sprintf(msg, "Loaded: %s (%lu bytes)", s->filename, (unsigned long)fsize);
    mp_set_status(s, msg);
}

// ─── Transport actions
// ────────────────────────────────────────────────────────

static void mp_action_play_pause(midiplayer_state_t *s) {
    switch (s->state) {
    case MIDI_STATE_IDLE:
        mp_set_status(s, "Load a file first.");
        break;
    case MIDI_STATE_LOADED:
        midi_player_play(s->looping);
        {
            extern volatile uint32_t pit_ticks;
            s->play_start_pit = pit_ticks;
        }
        s->state = MIDI_STATE_PLAYING;
        mp_set_status(s, "Playing.");
        break;
    case MIDI_STATE_PLAYING:
        midi_player_pause();
        s->state = MIDI_STATE_PAUSED;
        mp_set_status(s, "Paused.");
        break;
    case MIDI_STATE_PAUSED:
        midi_player_pause(); // toggle resumes
        s->state = MIDI_STATE_PLAYING;
        mp_set_status(s, "Resumed.");
        break;
    }
}

static void mp_action_stop(midiplayer_state_t *s) {
    if (s->state == MIDI_STATE_PLAYING || s->state == MIDI_STATE_PAUSED) {
        midi_player_stop();
        s->state = MIDI_STATE_LOADED;
        mp_set_status(s, "Stopped.");
    }
}

static void mp_action_rewind(midiplayer_state_t *s) {
    if (s->state == MIDI_STATE_IDLE)
        return;
    bool was_playing = (s->state == MIDI_STATE_PLAYING);
    midi_player_stop();
    if (s->midi_buf)
        midi_player_load(s->midi_buf, s->midi_size);
    if (was_playing) {
        midi_player_play(s->looping);
        extern volatile uint32_t pit_ticks;
        s->play_start_pit = pit_ticks;
        s->state = MIDI_STATE_PLAYING;
    } else {
        s->state = MIDI_STATE_LOADED;
    }
    for (int i = 0; i < VU_BARS; i++) {
        s->vu_peaks[i] = 0;
        s->vu_decay[i] = 0;
    }
    mp_set_status(s, "Rewound.");
}

static void mp_apply_volume(midiplayer_state_t *s) {
    uint8_t v = (uint8_t)(s->volume * 255 / 100);
    sb16_set_volume(v, v);
    opl2_set_master_volume(s->volume);
    // set FM synthesis volume on the SB16 mixer
    mixer_write(0x26, (v & 0xF0) | (v >> 4));
}

// ─── Active-instance helper
// ───────────────────────────────────────────────────

static midiplayer_state_t *active_mp(void) {
    window *fw = wm_focused_window();
    if (!fw)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &midiplayer_app)
            continue;
        midiplayer_state_t *s = (midiplayer_state_t *)a->state;
        if (s->win == fw)
            return s;
    }
    return NULL;
}

// ─── Menu callbacks
// ───────────────────────────────────────────────────────────

static void menu_play_pause(void) {
    midiplayer_state_t *s = active_mp();
    if (!s)
        return;
    mp_action_play_pause(s);
}
static void menu_stop(void) {
    midiplayer_state_t *s = active_mp();
    if (!s)
        return;
    mp_action_stop(s);
}
static void menu_rewind(void) {
    midiplayer_state_t *s = active_mp();
    if (!s)
        return;
    mp_action_rewind(s);
}
static void menu_toggle_loop(void) {
    midiplayer_state_t *s = active_mp();
    if (!s)
        return;
    s->looping = !s->looping;
    if (s->state == MIDI_STATE_PLAYING)
        midi_player_set_loop(s->looping);
    mp_set_status(s, s->looping ? "Loop ON." : "Loop OFF.");
}
static void menu_vol_up(void) {
    midiplayer_state_t *s = active_mp();
    if (!s)
        return;
    if (s->volume < 100) {
        s->volume += 10;
        if (s->volume > 100)
            s->volume = 100;
    }
    mp_apply_volume(s);
}
static void menu_vol_down(void) {
    midiplayer_state_t *s = active_mp();
    if (!s)
        return;
    if (s->volume > 0) {
        if (s->volume < 10)
            s->volume = 0;
        else
            s->volume -= 10;
    }
    mp_apply_volume(s);
}
static void menu_close_mp(void) {
    midiplayer_state_t *s = active_mp();
    if (!s)
        return;
    os_close_own_instance(s->win);
}
static void on_about_mp(void) {
    modal_show(MODAL_INFO, "About MIDI Player",
               "MIDI Player v1.0\nASMOS OPL2/FM Synthesizer\nSupports Type-0 "
               "and Type-1 MIDI",
               NULL, NULL);
}

// ─── App lifecycle
// ────────────────────────────────────────────────────────────

static bool midiplayer_close(window *w) {
    midiplayer_state_t *s = NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &midiplayer_app)
            continue;
        midiplayer_state_t *ms = (midiplayer_state_t *)a->state;
        if (ms->win == w) {
            s = ms;
            break;
        }
    }
    if (s) {
        midi_player_stop();
        if (s->midi_buf) {
            kfree(s->midi_buf);
            s->midi_buf = NULL;
        }
    }
    os_close_own_instance(w);
    return true;
}

static void midiplayer_init(void *state) {
    midiplayer_state_t *s = (midiplayer_state_t *)state;

    const window_spec_t spec = {
        .x = MIDI_DEFAULT_X,
        .y = MIDI_DEFAULT_Y,
        .w = MIDI_DEFAULT_W,
        .h = MIDI_DEFAULT_H,
        .min_w = MIDI_DEFAULT_W,
        .min_h = MIDI_DEFAULT_H,
        .resizable = false,
        .title = "MIDI Player",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = BLACK,
        .visible = true,
        .on_close = midiplayer_close,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;

    s->win->on_draw = midiplayer_draw;
    s->win->on_draw_userdata = s;

    menu *m = window_add_menu(s->win, "Player");
    menu_add_item(m, "Play / Pause", menu_play_pause);
    menu_add_item(m, "Stop", menu_stop);
    menu_add_item(m, "Rewind", menu_rewind);
    menu_add_separator(m);
    menu_add_item(m, "Toggle Loop", menu_toggle_loop);
    menu_add_separator(m);
    menu_add_item(m, "Volume +10", menu_vol_up);
    menu_add_item(m, "Volume -10", menu_vol_down);
    menu_add_separator(m);
    menu_add_item(m, "Close", menu_close_mp);
    menu_add_item(m, "About", on_about_mp);

    s->state = MIDI_STATE_IDLE;
    s->volume = 80;
    s->looping = false;
    s->path_len = 0;
    s->path_buf[0] = '\0';
    strncpy(s->path_buf, "/MUSIC.MID", MAX_PATH_LEN);
    s->path_len = (int)strlen(s->path_buf);

    for (int i = 0; i < VU_BARS; i++) {
        s->vu_peaks[i] = 0;
        s->vu_decay[i] = 0;
    }

    mp_apply_volume(s);
}

static void midiplayer_on_frame(void *state) {
    midiplayer_state_t *s = (midiplayer_state_t *)state;
    if (!s->win || !s->win->visible)
        return;

    bool focused = window_is_focused(s->win);

    // Advance MIDI sequencer (no-op if not playing)
    midi_player_update();

    // Detect natural end of playback
    if (s->state == MIDI_STATE_PLAYING && !midi_player_is_playing()) {
        s->state = MIDI_STATE_LOADED;
        for (int i = 0; i < VU_BARS; i++)
            s->vu_peaks[i] = 0;
        mp_set_status(s, "Playback finished.");
    }

    if (s->win->minimized)
        return;

    // VU meter animation
    mp_update_vu(s);

    if (s->status_timer > 0)
        s->status_timer--;

    if (focused && mouse.left_clicked) {
        int cx = win_cx(s);
        int cy = win_cy(s);

        // ── Path input click ─────────────────────────────────────────────
        int py = cy + PATH_Y;
        if (btn_hit(mouse.x, mouse.y, cx + PATH_X, py, PATH_W, PATH_H)) {
            s->path_focused = true;
        } else if (btn_hit(mouse.x, mouse.y, cx + LOAD_BTN_X, py, LOAD_BTN_W,
                           PATH_H)) {
            s->path_focused = false;
            mp_load_file(s);
        } else {
            s->path_focused = false;
        }

        // ── Transport buttons ─────────────────────────────────────────────
        int tx = cx + PAD;
        int ty = cy + TRANS_Y;

        // Rewind
        if (btn_hit(mouse.x, mouse.y, tx, ty, BTN_SZ, BTN_SZ)) {
            mp_action_rewind(s);
            midiplayer_draw(s->win, s);
        }
        tx += BTN_SZ + 3;

        // Play/Pause
        if (btn_hit(mouse.x, mouse.y, tx, ty, BTN_SZ, BTN_SZ)) {
            mp_action_play_pause(s);
            midiplayer_draw(s->win, s);
        }
        tx += BTN_SZ + 3;

        // Stop
        if (btn_hit(mouse.x, mouse.y, tx, ty, BTN_SZ, BTN_SZ)) {
            mp_action_stop(s);
            midiplayer_draw(s->win, s);
        }
        tx += BTN_SZ + 3;

        // Loop
        if (btn_hit(mouse.x, mouse.y, tx, ty, LOOP_BTN_W, BTN_SZ)) {
            menu_toggle_loop();
            midiplayer_draw(s->win, s);
        }

        // ── Volume track click/drag ───────────────────────────────────────
        int vol_y = cy + VOL_Y;
        int track_x = cx + VOL_TRACK_X;
        if (btn_hit(mouse.x, mouse.y, track_x, vol_y - 2, VOL_TRACK_W,
                    VOL_H + 4)) {
            s->vol_dragging = true;
        }
    }

    // Volume drag
    if (s->vol_dragging) {
        if (mouse.left) {
            int cx = win_cx(s);
            int track_x = cx + VOL_TRACK_X;
            int rel = mouse.x - track_x;
            if (rel < 0)
                rel = 0;
            if (rel > VOL_TRACK_W)
                rel = VOL_TRACK_W;
            s->volume = (uint8_t)((uint32_t)rel * 100 / VOL_TRACK_W);
            mp_apply_volume(s);
        } else {
            s->vol_dragging = false;
        }
    }

    // ── Keyboard shortcuts ────────────────────────────────────────────────
    if (focused && kb.key_pressed) {
        if (s->path_focused) {
            if (kb.last_char && kb.last_char != '\b' && kb.last_char != '\r') {
                if (s->path_len < MAX_PATH_LEN) {
                    s->path_buf[s->path_len++] = kb.last_char;
                    s->path_buf[s->path_len] = '\0';
                }
            } else if (kb.last_scancode == BACKSPACE && s->path_len > 0) {
                s->path_buf[--s->path_len] = '\0';
            } else if (kb.last_char == '\r') {
                s->path_focused = false;
                mp_load_file(s);
            }
        } else {
            switch (kb.last_char) {
            case ' ':
                mp_action_play_pause(s);
                break;
            case 's':
            case 'S':
                mp_action_stop(s);
                break;
            case 'r':
            case 'R':
                mp_action_rewind(s);
                break;
            case 'l':
            case 'L':
                menu_toggle_loop();
                break;
            case '+':
                menu_vol_up();
                break;
            case '-':
                menu_vol_down();
                break;
            }
        }
    }
}

static void midiplayer_destroy(void *state) {
    midiplayer_state_t *s = (midiplayer_state_t *)state;
    midi_player_stop();
    if (s->midi_buf) {
        kfree(s->midi_buf);
        s->midi_buf = NULL;
    }
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor midiplayer_app = {
    .name = "MIDI Player",
    .state_size = sizeof(midiplayer_state_t),
    .init = midiplayer_init,
    .on_frame = midiplayer_on_frame,
    .destroy = midiplayer_destroy,
};
