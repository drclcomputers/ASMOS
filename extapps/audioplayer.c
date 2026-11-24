// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "extapps/shim.h"

#define MAX_LIST 40  /* (a CD has up to 99 tracks; most have under 25) */
#define PATH_LEN 192 /* long names */
#define BARS 24
#define STATUS_MS 4000

typedef struct {
    window *win; /* must stay first */
    control *viz, *bar, *play, *stop, *rew, *next, *loop, *vol, *vol_lbl, *list,
        *status, *cd;
    char paths[MAX_LIST][PATH_LEN];
    int count, cur; /* cur: playing / loaded, -1 none */
    bool looping;
    int volume;
    uint8_t peaks[BARS];
    uint32_t frame, shown_s, status_until;
    int shown_state;
    bool batch; /* several changes to the list: saved once, at the end */
} ap_state_t;

static const char *base_of(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* a CD track's file: Track07.cda */
static bool is_cda(const char *p) {
    const char *d = strrchr(p, '.');
    return d && !strcasecmp(d, ".cda");
}

/* what the list and the display call it: "CD track 7" for a .cda */
static const char *shown_name(const char *p, char *b, int n) {
    const char *base = base_of(p);
    if (is_cda(p) && !strncasecmp(base, "Track", 5)) {
        snprintf(b, n, "CD track %d", str_to_int(base + 5));
        return b;
    }
    return base;
}

static void say(ap_state_t *s, const char *msg) {
    ctl_set_text(s->status, msg);
    s->status_until = pit_ticks + STATUS_MS;
}

static void save_prefs(ap_state_t *s) {
    if (s->batch)
        return;
    app_prefs_begin("AUDIOPLA");
    for (int i = 0; i < MAX_LIST; i++) {
        char k[8];
        snprintf(k, sizeof(k), "p%d", i);
        app_prefs_set("AUDIOPLA", k, i < s->count ? s->paths[i] : "");
    }
    app_prefs_set_int("AUDIOPLA", "volume", s->volume);
    app_prefs_set_int("AUDIOPLA", "loop", s->looping);
    app_prefs_end();
}

// the list

static void fill_list(ap_state_t *s) {
    list_clear(s->list);
    for (int i = 0; i < s->count; i++) {
        char t[PATH_LEN + 8], nb[24];
        snprintf(t, sizeof(t), "%s%s", i == s->cur ? "> " : "   ",
                 shown_name(s->paths[i], nb, sizeof(nb)));
        list_add(s->list, t);
    }
    if (s->cur >= 0)
        list_set_sel(s->list, s->cur);
}

static void sync_ui(ap_state_t *s) {
    int st = audio_state();
    ctl_set_text(s->play, st == AUDIO_PLAYING ? "Pause" : "Play");
    ctl_enable(s->play, st != AUDIO_EMPTY || s->count > 0);
    ctl_enable(s->stop, st == AUDIO_PLAYING || st == AUDIO_PAUSED);
    ctl_enable(s->rew, st != AUDIO_EMPTY);
    ctl_enable(s->next, s->count > 1);
    ctl_set_value(s->loop, s->looping);
    ctl_set_value(s->vol, s->volume);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", s->volume);
    ctl_set_text(s->vol_lbl, b);
    s->shown_state = st;
    wm_invalidate(s->win);
}

/* load list item i; play it if `play` */
static bool load_item(ap_state_t *s, int i, bool play) {
    if (i < 0 || i >= s->count)
        return false;
    const char *err = audio_open(s->paths[i]);
    s->cur = i;
    fill_list(s);
    if (err) {
        char m[96], nb[24];
        snprintf(m, sizeof(m), "%s: %s.",
                 shown_name(s->paths[i], nb, sizeof(nb)), err);
        say(s, m);
        sync_ui(s);
        return false;
    }
    audio_set_loop(s->looping);
    if (play)
        audio_play();
    say(s, play ? "Playing." : "Loaded.");
    sync_ui(s);
    return true;
}

static int add_path(ap_state_t *s, const char *path) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->paths[i], path))
            return i; /* already there */
    if (s->count >= MAX_LIST) {
        say(s, "The list is full (40).");
        return -1;
    }
    strncpy(s->paths[s->count], path, PATH_LEN - 1);
    s->paths[s->count][PATH_LEN - 1] = 0;
    s->count++;
    fill_list(s);
    save_prefs(s);
    return s->count - 1;
}

static void remove_item(ap_state_t *s, int i) {
    if (i < 0 || i >= s->count)
        return;
    if (i == s->cur) {
        audio_close();
        s->cur = -1;
    } else if (i < s->cur) {
        s->cur--;
    }
    for (int k = i; k < s->count - 1; k++)
        memcpy(s->paths[k], s->paths[k + 1], PATH_LEN);
    s->count--;
    fill_list(s);
    save_prefs(s);
    sync_ui(s);
}

// actions

static void act_play(ap_state_t *s) {
    int st = audio_state();
    if (st == AUDIO_EMPTY) {
        int i = list_sel(s->list);
        load_item(s, i >= 0 ? i : 0, true);
        return;
    }
    if (st == AUDIO_PLAYING || st == AUDIO_PAUSED)
        audio_pause();
    else
        audio_play();
    say(s, audio_state() == AUDIO_PAUSED ? "Paused." : "Playing.");
    sync_ui(s);
}

static void act_stop(ap_state_t *s) {
    audio_stop();
    say(s, "Stopped.");
    sync_ui(s);
}

static void act_rewind(ap_state_t *s) {
    bool playing = audio_state() == AUDIO_PLAYING;
    audio_stop();
    if (playing)
        audio_play();
    say(s, "From the start.");
    sync_ui(s);
}

/* the audio CD in the drive: its tracks replace the CD tracks in the list
 * (an earlier disc's), and the first one plays */
static void act_cd(ap_state_t *s) {
    static app_dirent_t d[99];
    int n = A->list_dir("/CD0", d, 99);
    if (n < 0) {
        say(s, "No CD in the drive.");
        return;
    }
    s->batch = true;
    for (int i = s->count - 1; i >= 0; i--) /* the old disc's tracks */
        if (is_cda(s->paths[i]))
            remove_item(s, i);
    int first = -1;
    for (int i = 0; i < n; i++) {
        if ((d[i].attr & APP_ATTR_DIR) || !is_cda(d[i].name))
            continue;
        char p[PATH_LEN];
        snprintf(p, sizeof(p), "/CD0/%s", d[i].name);
        int k = add_path(s, p);
        if (k < 0)
            break; /* (full) */
        if (first < 0)
            first = k;
    }
    s->batch = false;
    save_prefs(s);
    if (first < 0) {
        say(s, "That CD has no audio tracks.");
        sync_ui(s);
        return;
    }
    load_item(s, first, true);
}

static void act_next(ap_state_t *s) {
    if (s->count)
        load_item(s, (s->cur + 1) % s->count, true);
}

static void act_loop(ap_state_t *s) {
    s->looping = !s->looping;
    audio_set_loop(s->looping);
    say(s, s->looping ? "Repeat this one: on." : "Repeat: off.");
    save_prefs(s);
    sync_ui(s);
}

static void set_volume(ap_state_t *s, int v) {
    s->volume = v < 0 ? 0 : v > 100 ? 100 : v;
    uint8_t x = (uint8_t)(s->volume * 255 / 100);
    sb16_set_volume(x, x);
    opl2_set_master_volume((uint8_t)s->volume);
    mixer_write(0x26, (uint8_t)((x & 0xF0) | (x >> 4)));
    mixer_write(0x28, (uint8_t)((x & 0xF0) | (x >> 4))); /* the CD cable */
}

// drawing

static void fmt_time(char *b, int n, uint32_t ms) {
    uint32_t sec = ms / 1000;
    snprintf(b, n, "%u:%02u", sec / 60, sec % 60);
}

static void paint_viz(control *c, int x, int y, int w, int h) {
    ap_state_t *s = c->user;
    fill_rect(x, y, w, h, BLACK);
    int f = UI_FONT_SMALL;
    if (audio_state() == AUDIO_EMPTY) {
        const char *t = s->count ? "Double-click a song in the list"
                                 : "Open a WAV or MIDI file, or play a CD";
        draw_string(x + (w - text_width(t, f)) / 2, y + h / 2 - 3, t, DARK_GRAY,
                    f);
        return;
    }
    char nb[24];
    draw_string(x + 3, y + 2, shown_name(audio_file(), nb, sizeof(nb)),
                LIGHT_CYAN, UI_FONT_BOLD);
    char info[64];
    snprintf(info, sizeof(info), "%s, %s", audio_codec_name(), audio_info());
    draw_string(x + 3, y + 12, info, LIGHT_GRAY, f);
    int bw = (w - 2) / BARS, bottom = y + h - 2, top = y + 22;
    for (int i = 0; i < BARS; i++) {
        int bh = s->peaks[i] * (bottom - top) / 255;
        if (bh < 1)
            bh = 1;
        uint8_t col = bh < (bottom - top) / 2        ? LIGHT_GREEN
                      : bh < (bottom - top) * 8 / 10 ? YELLOW
                                                     : LIGHT_RED;
        fill_rect(x + 1 + i * bw, bottom - bh, bw - 1, bh, col);
    }
}

static void paint_bar(control *c, int x, int y, int w, int h) {
    (void)c;
    fill_rect(x, y, w, h, WHITE);
    draw_rect(x, y, w, h, DARK_GRAY);
    uint32_t len = audio_len_ms(), pos = audio_pos_ms();
    int tw = 70; /* the time on the right */
    int bw = w - tw - 4;
    if (len) {
        int fw = (int)((uint32_t)(bw - 2) * (pos / 16) / (len / 16 + 1));
        fill_rect(x + 2, y + 2, fw, h - 4, BLUE);
    }
    char a[12], b[12], t[28];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), len);
    snprintf(t, sizeof(t), "%s / %s", a, b);
    int f = UI_FONT_SMALL;
    draw_string(x + w - 3 - text_width(t, f), y + (h - font_height(f)) / 2 + 1,
                t, BLACK, f);
}

/* click on the bar: go there (WAV, CD) */
static void on_bar(control *c, int ev) {
    ap_state_t *s = c->user;
    if (ev != EV_MOUSEDOWN || !audio_len_ms())
        return;
    int bw = c->aw - 70 - 4 - 2;
    if (c->mx < 2 || c->mx > bw + 2)
        return;
    if (!audio_seek_ms((uint32_t)(c->mx - 2) * audio_len_ms() / (uint32_t)bw))
        say(s, "MIDI plays from the start only.");
    sync_ui(s);
}

/* the meter: WAV from the samples; MIDI and CD drawn from the beat of time
 * (CD audio never passes through here) */
static void update_meter(ap_state_t *s) {
    int st = audio_state();
    bool moving = false;
    int lvl = audio_level();
    s->frame++;
    for (int i = 0; i < BARS; i++) {
        int target = 0;
        if (st == AUDIO_PLAYING) {
            uint32_t ph = s->frame * (uint32_t)(i + 3);
            int wave = (int)((ph ^ (ph >> 4) ^ (ph >> 7)) & 0xFF);
            int shape = i < 6 ? 200 + i * 8 : 250 - (i - 6) * 9;
            target = !strcmp(audio_codec_name(), "WAV")
                         ? lvl * (128 + wave / 2) / 255 * shape / 255
                         : wave * shape / 255;
            target = target * s->volume / 100;
        }
        if (target > s->peaks[i])
            s->peaks[i] = (uint8_t)target;
        else if (s->peaks[i])
            s->peaks[i] = (uint8_t)(s->peaks[i] > 12 ? s->peaks[i] - 12 : 0);
        moving |= s->peaks[i] != 0 || target;
    }
    if (moving)
        wm_invalidate(s->win);
}

// controls and menus

static void got_file(void *user, const char *path) {
    ap_state_t *s = user;
    if (!path)
        return;
    int i = add_path(s, path);
    if (i >= 0)
        load_item(s, i, true);
}

static void got_add(void *user, const char *path) {
    ap_state_t *s = user;
    if (path && add_path(s, path) >= 0) {
        say(s, "Added to the list.");
        sync_ui(s);
    }
}

enum {
    B_OPEN = 1,
    B_REW,
    B_PLAY,
    B_STOP,
    B_NEXT,
    B_LOOP,
    B_ADD,
    B_REMOVE,
    B_CD
};

static void on_button(control *c, int ev) {
    ap_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    switch (c->id) {
    case B_OPEN:
        dlg_open_file(s->win, "Open a sound", "/", "", got_file, s);
        break;
    case B_ADD:
        dlg_open_file(s->win, "Add to the list", "/", "", got_add, s);
        break;
    case B_REMOVE:
        remove_item(s, list_sel(s->list));
        break;
    case B_REW:
        act_rewind(s);
        break;
    case B_PLAY:
        act_play(s);
        break;
    case B_STOP:
        act_stop(s);
        break;
    case B_NEXT:
        act_next(s);
        break;
    case B_LOOP:
        act_loop(s);
        break;
    case B_CD:
        act_cd(s);
        break;
    }
}

static void on_list(control *c, int ev) {
    ap_state_t *s = c->user;
    if (ev == EV_DBLCLICK || (ev == EV_KEY && kb.last_scancode == ENTER))
        load_item(s, list_sel(c), true);
    else if (ev == EV_KEY && kb.last_scancode == DELETE)
        remove_item(s, list_sel(c));
}

static void on_vol(control *c, int ev) {
    if (ev == EV_CHANGE) {
        set_volume(c->user, ctl_value(c));
        save_prefs(c->user);
        sync_ui(c->user);
    }
}

#define MENU_ACTION(name, body)                                                \
    static void name(void) {                                                   \
        ap_state_t *s = ui_active_state();                                     \
        if (s)                                                                 \
            body;                                                              \
    }
MENU_ACTION(m_open, dlg_open_file(s->win, "Open a sound", "/", "", got_file, s))
MENU_ACTION(m_add,
            dlg_open_file(s->win, "Add to the list", "/", "", got_add, s))
MENU_ACTION(m_remove, remove_item(s, list_sel(s->list)))
MENU_ACTION(m_play, act_play(s))
MENU_ACTION(m_stop, act_stop(s))
MENU_ACTION(m_rewind, act_rewind(s))
MENU_ACTION(m_next, act_next(s))
MENU_ACTION(m_loop, act_loop(s))
MENU_ACTION(m_cd, act_cd(s))
MENU_ACTION(m_close, os_close_own_instance(s->win))

static void m_about(void) {
    dlg_message(NULL, "About Audio Player",
                "Audio Player 3.1\nWAV on the Sound Blaster, MIDI on the "
                "FM synth,\naudio CDs from the drive (its cable to the "
                "card).\nSpace play/pause, S stop, R start, N next,\n"
                "L repeat, C play the CD, +/- volume.\nClick the bar to "
                "jump (WAV, CD).");
}

static void ap_init(void *state) {
    ap_state_t *s = state;
    s->win = app_window(s, "Audio Player", 20, 14, 262, 176, DARK_GRAY, 0);
    if (!s->win)
        return;
    s->win->lazy_custom = true; /* meter and bar: invalidated in on_frame */
    window *w = s->win;
    s->viz = ctl_custom(w, 4, 4, 252, 42, paint_viz, NULL, CS_BORDER);
    s->bar = ctl_custom(w, 4, 50, 252, 12, paint_bar, on_bar, 0);
    ctl_set_tooltip(s->bar, "Click to jump there (WAV, CD)");
    s->rew = ctl_button(w, 4, 66, 22, 14, "|<", B_REW, on_button);
    s->play = ctl_button(w, 28, 66, 38, 14, "Play", B_PLAY, on_button);
    s->stop = ctl_button(w, 68, 66, 32, 14, "Stop", B_STOP, on_button);
    s->next = ctl_button(w, 102, 66, 22, 14, ">|", B_NEXT, on_button);
    s->loop = ctl_button(w, 126, 66, 36, 14, "Repeat", B_LOOP, on_button);
    ctl_style_add(s->loop, CS_TOGGLE);
    ctl_set_tooltip(s->rew, "Back to the start (R)");
    ctl_set_tooltip(s->next, "Next in the list (N)");
    ctl_set_tooltip(s->loop, "Play this one again and again (L)");
    s->vol = ctl_slider(w, 166, 67, 62, 0, 100, 80, on_vol);
    s->vol_lbl = ctl_label(w, 230, 66, 26, 14, "", LS_RIGHT);
    s->list = ctl_list(w, 4, 84, 252, 58, 0, on_list);
    control *o = ctl_button(w, 4, 146, 44, 13, "Open...", B_OPEN, on_button);
    ctl_set_tooltip(o, "Open and play; it joins the list");
    ctl_button(w, 50, 146, 36, 13, "Add...", B_ADD, on_button);
    ctl_button(w, 88, 146, 40, 13, "Remove", B_REMOVE, on_button);
    s->cd = ctl_button(w, 130, 146, 36, 13, "CD", B_CD, on_button);
    ctl_set_tooltip(s->cd, "Play the audio CD in the drive (C)");
    s->status = ctl_label(w, 168, 146, 88, 13, "", LS_RIGHT);

    menu *m = window_add_menu(w, "Player");
    menu_add_item(m, "Open...", m_open);
    menu_add_item(m, "Add to List...", m_add);
    menu_add_item(m, "Remove from List", m_remove);
    menu_add_item(m, "Play Audio CD  C", m_cd);
    menu_add_separator(m);
    menu_add_item(m, "Play / Pause  Space", m_play);
    menu_add_item(m, "Stop  S", m_stop);
    menu_add_item(m, "From the Start  R", m_rewind);
    menu_add_item(m, "Next  N", m_next);
    menu_add_item(m, "Repeat  L", m_loop);
    menu_add_separator(m);
    menu_add_item(m, "About Audio Player", m_about);
    menu_add_item(m, "Close", m_close);

    s->cur = -1;
    for (int i = 0; i < MAX_LIST; i++) {
        char k[8];
        snprintf(k, sizeof(k), "p%d", i);
        if (app_prefs_get("AUDIOPLA", k, s->paths[s->count], PATH_LEN) &&
            s->paths[s->count][0])
            s->count++;
    }
    s->looping = app_prefs_get_int("AUDIOPLA", "loop", 0) != 0;
    set_volume(s, app_prefs_get_int("AUDIOPLA", "volume", 80));
    fill_list(s);
    if (!sb16_detected() && !opl2_detected())
        say(s, "No sound card found.");
    sync_ui(s);
}

static void ap_frame(void *state) {
    ap_state_t *s = state;
    if (!s->win)
        return;
    char req[FS_PATH_SZ]; /* FileF / the desktop: play this file */
    if (app_open_request(req, sizeof(req))) {
        got_file(s, req);
        wm_focus(s->win);
    }
    if (audio_finished()) { /* the end: the next in the list, if any */
        if (s->cur >= 0 && s->cur + 1 < s->count)
            load_item(s, s->cur + 1, true);
        else
            say(s, "Finished.");
        sync_ui(s);
    }
    if (audio_state() != s->shown_state)
        sync_ui(s);
    update_meter(s);
    uint32_t sec = audio_pos_ms() / 500;
    if (sec != s->shown_s) { /* the bar: twice a second */
        s->shown_s = sec;
        wm_invalidate(s->win);
    }
    if (s->status_until && pit_ticks > s->status_until) {
        ctl_set_text(s->status, "");
        s->status_until = 0;
    }
    if (!kb.key_pressed || !window_is_focused(s->win) ||
        ctl_focused(s->win) == s->list)
        return;
    switch (kb.last_char) {
    case ' ':
        act_play(s);
        break;
    case 's':
    case 'S':
        act_stop(s);
        break;
    case 'r':
    case 'R':
        act_rewind(s);
        break;
    case 'n':
    case 'N':
        act_next(s);
        break;
    case 'l':
    case 'L':
        act_loop(s);
        break;
    case 'c':
    case 'C':
        act_cd(s);
        break;
    case '+':
        set_volume(s, s->volume + 10);
        sync_ui(s);
        break;
    case '-':
        set_volume(s, s->volume - 10);
        sync_ui(s);
        break;
    }
}

static void ap_destroy(void *state) {
    (void)state;
    audio_close();
}

APP_META("Audio Player", ap_state_t, true, 0, ap_init, ap_frame, ap_destroy);
