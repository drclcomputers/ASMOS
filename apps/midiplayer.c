/* MIDI Player - plays .MID files through the OPL2 FM synth. */
#include "drivers/opl2.h"
#include "drivers/sb16.h"
#include "os/api.h"

#define VU_BARS 24
#define VU_BAR_MAXH 46
#define MAX_PATH_LEN 63
#define MAX_MIDI_SIZE (64 * 1024)
#define STATUS_MS 4000

typedef enum {
    MIDI_STATE_IDLE,
    MIDI_STATE_LOADED,
    MIDI_STATE_PLAYING,
    MIDI_STATE_PAUSED,
} midi_ui_state_t;

typedef struct {
    window *win; /* must stay first */
    control *path, *viz, *play, *stop, *rew, *loop, *state_lbl, *vol, *vol_lbl,
        *status;

    char path_buf[MAX_PATH_LEN + 1];
    int path_len;

    uint8_t *midi_buf;
    uint32_t midi_size;
    char filename[32];

    midi_ui_state_t state;
    bool looping;
    uint8_t volume;

    uint8_t vu_peaks[VU_BARS];
    uint8_t vu_decay[VU_BARS];
    uint32_t vu_frame;

    uint32_t play_start_pit;
    uint32_t status_until;
} midiplayer_state_t;

app_descriptor midiplayer_app;

static void mp_set_status(midiplayer_state_t *s, const char *msg) {
    ctl_set_text(s->status, msg);
    s->status_until = time_millis() + STATUS_MS;
}

// meter update
// fake btw
static void mp_update_vu(midiplayer_state_t *s) {
    if (s->state != MIDI_STATE_PLAYING) {
        for (int i = 0; i < VU_BARS; i++) {
            if (s->vu_peaks[i] > 0)
                s->vu_peaks[i] = s->vu_peaks[i] > 3 ? s->vu_peaks[i] - 3 : 0;
        }
        return;
    }

    s->vu_frame++;

    uint32_t t = s->vu_frame;
    for (int i = 0; i < VU_BARS; i++) {
        uint32_t phase = t * (uint32_t)(i + 3);
        uint8_t wave = (uint8_t)((phase ^ (phase >> 4) ^ (phase >> 7)) & 0xFF);
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

        target = (uint8_t)((target * VU_BAR_MAXH) >> 8);

        target = (uint8_t)((target * (uint32_t)s->volume) / 100);

        if (target > s->vu_peaks[i]) {
            s->vu_peaks[i] = target;
        } else {
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

// File loading
static void mp_load_file(midiplayer_state_t *s) {
    if (s->path_len == 0) {
        mp_set_status(s, "No path entered.");
        return;
    }

    // stop current playback
    if (s->state == MIDI_STATE_PLAYING || s->state == MIDI_STATE_PAUSED) {
        midi_player_stop();
        s->state = MIDI_STATE_IDLE;
    }

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

    const char *slash = NULL;
    for (int i = 0; i < s->path_len; i++)
        if (s->path_buf[i] == '/' || s->path_buf[i] == '\\')
            slash = s->path_buf + i;
    const char *base = slash ? slash + 1 : s->path_buf;
    strncpy(s->filename, base, 31);
    s->filename[31] = '\0';

    // Reset VU
    for (int i = 0; i < VU_BARS; i++) {
        s->vu_peaks[i] = 0;
        s->vu_decay[i] = 0;
    }

    char msg[64];
    sprintf(msg, "Loaded: %s (%lu bytes)", s->filename, (unsigned long)fsize);
    mp_set_status(s, msg);
}

static void mp_action_play_pause(midiplayer_state_t *s) {
    switch (s->state) {
    case MIDI_STATE_IDLE:
        mp_set_status(s, "Load a file first.");
        break;
    case MIDI_STATE_LOADED:
        midi_player_play(s->looping);
        {
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
        midi_player_pause();
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
    mixer_write(0x26, (v & 0xF0) | (v >> 4));
}

static void sync_ui(midiplayer_state_t *s) {
    static const char *names[] = {"IDLE", "READY", "PLAYING", "PAUSED"};
    static const uint8_t cols[] = {DARK_GRAY, BLACK, GREEN, BROWN};
    ctl_set_text(s->state_lbl, names[s->state]);
    ctl_set_colors(s->state_lbl, cols[s->state], 0xFF);
    ctl_set_text(s->play, s->state == MIDI_STATE_PLAYING ? "Pause" : "Play");
    ctl_enable(s->play, s->state != MIDI_STATE_IDLE);
    ctl_enable(s->rew, s->state != MIDI_STATE_IDLE);
    ctl_enable(s->stop,
               s->state == MIDI_STATE_PLAYING || s->state == MIDI_STATE_PAUSED);
    ctl_set_value(s->loop, s->looping);
    ctl_set_value(s->vol, s->volume);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", s->volume);
    ctl_set_text(s->vol_lbl, b);
}

static void load_path(midiplayer_state_t *s, const char *p) {
    strncpy(s->path_buf, p, MAX_PATH_LEN);
    s->path_buf[MAX_PATH_LEN] = 0;
    s->path_len = (int)strlen(s->path_buf);
    ctl_set_text(s->path, s->path_buf);
    mp_load_file(s);
    sync_ui(s);
}

static void toggle_loop(midiplayer_state_t *s) {
    s->looping = !s->looping;
    if (s->state == MIDI_STATE_PLAYING)
        midi_player_set_loop(s->looping);
    mp_set_status(s, s->looping ? "Loop on." : "Loop off.");
}

static void set_volume(midiplayer_state_t *s, int v) {
    s->volume = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    mp_apply_volume(s);
}

/* ── visualiser ──────────────────────────────────────────────────────── */

static void paint_viz(control *c, int x, int y, int w, int h) {
    midiplayer_state_t *s = c->user;
    fill_rect(x, y, w, h, BLACK);
    if (s->state == MIDI_STATE_IDLE) {
        draw_string(x + 8, y + h / 2 - 3, "No file loaded", DARK_GRAY, 2);
        return;
    }
    draw_string(x + 3, y + 3, s->filename, LIGHT_CYAN, 2);
    int bw = (w - 2) / VU_BARS;
    int bottom = y + h - 2;
    for (int i = 0; i < VU_BARS; i++) {
        int bh = s->vu_peaks[i] * (h - 14) / VU_BAR_MAXH;
        if (bh < 1)
            bh = 1;
        uint8_t col = bh < (h - 14) / 2        ? LIGHT_GREEN
                      : bh < (h - 14) * 8 / 10 ? YELLOW
                                               : LIGHT_RED;
        fill_rect(x + 1 + i * bw, bottom - bh, bw - 1, bh, col);
    }
}

/* ── controls ────────────────────────────────────────────────────────── */

static void got_file(void *user, const char *path) {
    if (path)
        load_path(user, path);
}

enum { B_BROWSE = 1, B_LOAD, B_REW, B_PLAY, B_STOP, B_LOOP };

static void on_button(control *c, int ev) {
    midiplayer_state_t *s = c->user;
    (void)ev;
    switch (c->id) {
    case B_BROWSE:
        dlg_open_file(s->win, "Open MIDI file", "/", "MID", got_file, s);
        return;
    case B_LOAD:
        load_path(s, ctl_text(s->path));
        return;
    case B_REW:
        mp_action_rewind(s);
        break;
    case B_PLAY:
        mp_action_play_pause(s);
        break;
    case B_STOP:
        mp_action_stop(s);
        break;
    case B_LOOP:
        toggle_loop(s);
        break;
    }
    sync_ui(s);
}

static void on_path(control *c, int ev) {
    if (ev == EV_SUBMIT)
        load_path(c->user, ctl_text(c));
}

static void on_vol(control *c, int ev) {
    if (ev == EV_CHANGE) {
        set_volume(c->user, ctl_value(c));
        sync_ui(c->user);
    }
}

/* ── menus ───────────────────────────────────────────────────────────── */

static void menu_open(void) {
    midiplayer_state_t *s = ui_active_state();
    if (s)
        dlg_open_file(s->win, "Open MIDI file", "/", "MID", got_file, s);
}
#define MENU_ACTION(name, body)                                                \
    static void name(void) {                                                   \
        midiplayer_state_t *s = ui_active_state();                             \
        if (!s)                                                                \
            return;                                                            \
        body;                                                                  \
        sync_ui(s);                                                            \
    }
MENU_ACTION(menu_play_pause, mp_action_play_pause(s))
MENU_ACTION(menu_stop, mp_action_stop(s))
MENU_ACTION(menu_rewind, mp_action_rewind(s))
MENU_ACTION(menu_loop, toggle_loop(s))
MENU_ACTION(menu_vol_up, set_volume(s, s->volume + 10))
MENU_ACTION(menu_vol_down, set_volume(s, s->volume - 10))
MENU_ACTION(menu_close, os_close_own_instance(s->win))

static void on_about(void) {
    dlg_message(NULL, "About MIDI Player",
                "MIDI Player v2.0\nASMOS OPL2/FM synthesizer\n"
                "Type-0 and Type-1 MIDI files.\n"
                "Keys: Space play/pause, S stop, R rewind, L loop, +/- vol.");
}

static void midiplayer_init(void *state) {
    midiplayer_state_t *s = state;
    s->win = app_window(s, "MIDI Player", 20, 20, 262, 146, DARK_GRAY, 0);
    if (!s->win)
        return;
    window *w = s->win;
    s->path = ctl_edit(w, 4, 4, 160, 12, "/MUSIC.MID", MAX_PATH_LEN,
                       ES_FILENAME, on_path);
    ctl_button(w, 168, 4, 48, 12, "Browse...", B_BROWSE, on_button);
    ctl_button(w, 220, 4, 36, 12, "Load", B_LOAD, on_button);
    s->viz = ctl_custom(w, 4, 20, 252, 52, paint_viz, NULL, CS_BORDER);
    s->rew = ctl_button(w, 4, 77, 30, 14, "|<", B_REW, on_button);
    s->play = ctl_button(w, 38, 77, 40, 14, "Play", B_PLAY, on_button);
    s->stop = ctl_button(w, 82, 77, 36, 14, "Stop", B_STOP, on_button);
    s->loop = ctl_button(w, 122, 77, 36, 14, "Loop", B_LOOP, on_button);
    ctl_style_add(s->loop, CS_TOGGLE);
    s->state_lbl = ctl_label(w, 170, 77, 86, 14, "", LS_RIGHT);
    ctl_label(w, 4, 96, 30, 11, "Volume", 0);
    s->vol = ctl_slider(w, 40, 96, 180, 0, 100, 80, on_vol);
    s->vol_lbl = ctl_label(w, 224, 96, 32, 11, "", LS_RIGHT);
    s->status = ctl_label(w, 4, 112, 252, 12, "", CS_BORDER);

    menu *m = window_add_menu(w, "Player");
    menu_add_item(m, "Open...", menu_open);
    menu_add_separator(m);
    menu_add_item(m, "Play / Pause", menu_play_pause);
    menu_add_item(m, "Stop", menu_stop);
    menu_add_item(m, "Rewind", menu_rewind);
    menu_add_item(m, "Toggle Loop", menu_loop);
    menu_add_separator(m);
    menu_add_item(m, "Volume +10", menu_vol_up);
    menu_add_item(m, "Volume -10", menu_vol_down);
    menu_add_separator(m);
    menu_add_item(m, "Close", menu_close);
    menu_add_item(m, "About", on_about);

    strcpy(s->path_buf, "/MUSIC.MID");
    s->path_len = (int)strlen(s->path_buf);
    s->state = MIDI_STATE_IDLE;
    s->volume = 80;
    mp_apply_volume(s);
    mp_set_status(s, "Browse... for a .MID file, or type a path + Enter.");
    s->status_until = 0xFFFFFFFFu;
    sync_ui(s);
}

static void midiplayer_on_frame(void *state) {
    midiplayer_state_t *s = state;
    midi_player_update();
    if (s->state == MIDI_STATE_PLAYING && !midi_player_is_playing()) {
        s->state = MIDI_STATE_LOADED;
        for (int i = 0; i < VU_BARS; i++)
            s->vu_peaks[i] = 0;
        mp_set_status(s, "Playback finished.");
        sync_ui(s);
    }
    mp_update_vu(s);
    if (time_millis() > s->status_until) {
        ctl_set_text(s->status, "");
        s->status_until = 0xFFFFFFFFu;
    }

    /* single-key shortcuts (keys the focused control didn't take) */
    if (!kb.key_pressed || !window_is_focused(s->win) ||
        ctl_focused(s->win) == s->path)
        return;
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
        toggle_loop(s);
        break;
    case '+':
        set_volume(s, s->volume + 10);
        break;
    case '-':
        set_volume(s, s->volume - 10);
        break;
    default:
        return;
    }
    sync_ui(s);
}

static void midiplayer_destroy(void *state) {
    midiplayer_state_t *s = state;
    midi_player_stop();
    if (s->midi_buf)
        kfree(s->midi_buf);
    s->midi_buf = NULL;
}

app_descriptor midiplayer_app = {
    .name = "MIDI Player",
    .state_size = sizeof(midiplayer_state_t),
    .init = midiplayer_init,
    .on_frame = midiplayer_on_frame,
    .destroy = midiplayer_destroy,
};
