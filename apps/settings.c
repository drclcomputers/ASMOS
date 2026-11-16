/* Settings - edits g_cfg; wallpaper/sound changes apply at once, Save writes
 * ASMOS.CFG. */
#include "config/runtime_config.h"
#include "config/video_cfg.h"
#include "drivers/sb16.h"
#include "lib/time.h"
#include "network/gopher.h" /* parse_ip_str */
#include "os/api.h"
#include "ui/saver.h"

enum { P_WALL, P_SOUND, P_SYSTEM, P_NET, P_SAVER, P_PERF, P_CLOCK };

#define PERF_FLAGS 6

typedef struct {
    window *win; /* must stay first */
    control *tabs, *status, *save;
    control *pattern, *col_a, *col_b, *screen;
    int screen_saved; /* mode on the boot disk (config/video_cfg.h) */
    control *vol, *vol_lbl;
    control *ip[4]; /* ip, gateway, netmask, dns */
    control *perf[PERF_FLAGS], *fps;
    /* Clock tab: the fields follow the clock until edited */
    control *c_day, *c_mon, *c_year, *c_h, *c_m, *c_s, *c_wd, *c_now;
    bool clock_edited;
    uint32_t clock_shown; /* second last shown */
    bool dirty;
} settings_state_t;

app_descriptor settings_app;

static const char *COLORS[16] = {
    "Black",     "Blue",          "Green",       "Cyan",
    "Red",       "Magenta",       "Brown",       "Light Gray",
    "Dark Gray", "Light Blue",    "Light Green", "Light Cyan",
    "Light Red", "Light Magenta", "Yellow",      "White"};
static const char *PATTERNS[5] = {"Solid", "Checkerboard", "Stripes",
                                  "Vertical stripes", "Dots"};

static void set_dirty(settings_state_t *s, bool d) {
    s->dirty = d;
    ctl_set_text(s->status, d ? "Unsaved changes" : "All saved");
    ctl_set_colors(s->status, d ? RED : DARK_GRAY, 0xFF);
}

/* ── wallpaper preview ─────────────────────────────────────────────────── */

static void paint_preview(control *c, int x, int y, int w, int h) {
    (void)c;
    uint8_t a = g_cfg.wallpaper_main_color, b = g_cfg.wallpaper_secondary_color;
    switch (g_cfg.wallpaper_pattern) {
    case WALLPAPER_CHECKERBOARD:
        for (int yy = 0; yy < h; yy += 4)
            for (int xx = 0; xx < w; xx += 4)
                fill_rect(x + xx, y + yy, 4, 4,
                          ((xx / 4) + (yy / 4)) % 2 ? b : a);
        break;
    case WALLPAPER_STRIPES:
        for (int yy = 0; yy < h; yy++)
            draw_line(x, y + yy, x + w - 1, y + yy, (yy / 2) % 2 ? b : a);
        break;
    case WALLPAPER_VERTICAL_STRIPES:
        for (int xx = 0; xx < w; xx++)
            draw_line(x + xx, y, x + xx, y + h - 1, (xx / 3) % 2 ? b : a);
        break;
    case WALLPAPER_DOTS:
        fill_rect(x, y, w, h, a);
        for (int yy = 2; yy < h; yy += 4)
            for (int xx = 2; xx < w; xx += 4)
                draw_dot(x + xx, y + yy, b);
        break;
    default:
        fill_rect(x, y, w, h, a);
    }
}

/* ── handlers ──────────────────────────────────────────────────────────── */

static void on_wall(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CHANGE)
        return;
    g_cfg.wallpaper_pattern = (uint8_t)ctl_value(s->pattern);
    g_cfg.wallpaper_main_color = (uint8_t)ctl_value(s->col_a);
    g_cfg.wallpaper_secondary_color = (uint8_t)ctl_value(s->col_b);
    set_dirty(s, true);
}

/* checkboxes store a pointer to their g_cfg byte in c->id */
static void on_flag(control *c, int ev) {
    if (ev != EV_CHANGE)
        return;
    *(uint8_t *)(uintptr_t)c->id = (uint8_t)ctl_value(c);
    set_dirty(c->user, true);
}

static void on_volume(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CHANGE)
        return;
    g_cfg.sb16_volume = (uint8_t)ctl_value(c);
    uint8_t v = (g_cfg.sb16_volume << 4) | g_cfg.sb16_volume;
    sb16_set_volume(v, v);
    char b[16];
    snprintf(b, sizeof(b), "%d / 15", g_cfg.sb16_volume);
    ctl_set_text(s->vol_lbl, b);
    set_dirty(s, true);
}

static void clock_fill(settings_state_t *s, bool fields);

static void on_tz(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CHANGE)
        return;
    g_cfg.timezone_offset = (uint8_t)(int8_t)(ctl_value(c) - 12);
    set_dirty(s, true);
    if (s->c_now) /* the local time moved */
        clock_fill(s, !s->clock_edited);
}

static void on_ffmode(control *c, int ev) {
    if (ev != EV_CHANGE)
        return;
    g_cfg.filef_single_window = (uint8_t)c->id;
    set_dirty(c->user, true);
}

static void on_screen(control *c, int ev) {
    if (ev == EV_CHANGE)
        set_dirty(c->user, true);
}

static void on_edit(control *c, int ev) {
    if (ev == EV_CHANGE)
        set_dirty(c->user, true);
}

static void on_save(control *c, int ev) {
    settings_state_t *s = c->user;
    (void)ev;
    static const char *names[4] = {"IP address", "Gateway", "Netmask",
                                   "DNS server"};
    uint8_t *dst[4] = {g_cfg.ip, g_cfg.gateway, g_cfg.netmask,
                       g_cfg.dns_server};
    uint8_t tmp[4][4];
    for (int i = 0; i < 4; i++)
        if (!parse_ip_str(ctl_text(s->ip[i]), tmp[i])) {
            char m[64];
            snprintf(m, sizeof(m), "%s must look like 10.0.2.15.", names[i]);
            ctl_set_value(s->tabs, P_NET);
            ctl_focus(s->ip[i]);
            dlg_message(s->win, "Settings", m);
            return;
        }
    for (int i = 0; i < 4; i++)
        memcpy(dst[i], tmp[i], 4);
    int vm = ctl_value(s->screen);
    bool screen_changed = vm != s->screen_saved;
    if (screen_changed) {
        const char *err = video_cfg_set(vm);
        if (err) {
            char m[96];
            snprintf(m, sizeof(m), "Screen mode not saved:\n%s.", err);
            dlg_message(s->win, "Settings", m);
            return;
        }
        s->screen_saved = vm;
    }
    if (cfg_save()) {
        set_dirty(s, false);
        ctl_set_text(s->status, screen_changed
                                    ? "Saved. Restart for the new screen."
                                    : "Saved. Network: applies on restart.");
    } else {
        dlg_message(s->win, "Settings", "Could not write ASMOS.CFG.");
    }
}

/* ── building the pages ────────────────────────────────────────────────── */

static control *on_page(settings_state_t *s, control *c, int page) {
    ctl_on_page(c, s->tabs, page);
    return c;
}

static control *flag(settings_state_t *s, int page, int x, int y,
                     const char *text, uint8_t *field) {
    control *c = on_page(
        s, ctl_check(s->win, x, y, 200, text, *field != 0, on_flag), page);
    c->id = (int)(uintptr_t)field;
    return c;
}

/* Speed tab: shown as "on" when the stored "off" flag is 0 */
static const struct {
    const char *text, *tip;
    uint8_t *off;
} PERF[PERF_FLAGS] = {
    {"Window animations", "Windows zoom open, closed and to the taskbar",
     &g_cfg.reduce_motion},
    {"Show windows while moving and resizing",
     "Off: only a frame moves; the window follows when you let go",
     &g_cfg.outline_drag},
    {"Seconds in the menu bar clock", "Off: the clock changes once a minute",
     &g_cfg.clock_no_secs},
    {"Blinking text cursor", "Off: the cursor stays on", &g_cfg.caret_no_blink},
    {"Tooltips", "Notes like this one", &g_cfg.no_tooltips},
    {"Shut down and restart animation", "Off: at once", &g_cfg.no_power_anim},
};

static void on_perf(control *c, int ev) {
    if (ev != EV_CHANGE || c->id < 0 || c->id >= PERF_FLAGS)
        return;
    *PERF[c->id].off = ctl_value(c) ? 0 : 1;
    set_dirty(c->user, true);
}

static void on_fps(control *c, int ev) {
    int i = ctl_value(c);
    if (ev != EV_CHANGE || i < 0 || i > 2)
        return;
    g_cfg.fps_cap = (uint8_t)i;
    set_dirty(c->user, true);
}

static void on_preset(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    cfg_set_low_end(c->id != 0);
    for (int i = 0; i < PERF_FLAGS; i++)
        ctl_set_value(s->perf[i], *PERF[i].off == 0);
    list_set_sel(s->fps, g_cfg.fps_cap);
    set_dirty(s, true);
}

static const int SAVER_DELAY[] = {0, 1, 2, 3, 5, 10, 15, 30};
#define SAVER_DELAYS ((int)(sizeof(SAVER_DELAY) / sizeof(SAVER_DELAY[0])))

static void paint_saver(control *c, int x, int y, int w, int h) {
    saver_paint(x, y, w, h, g_cfg.saver_effect);
    wm_invalidate(c->win); /* animated: draw again next frame */
}

static void on_saver_fx(control *c, int ev) {
    if (ev != EV_CHANGE)
        return;
    g_cfg.saver_effect = (uint8_t)ctl_value(c);
    set_dirty(c->user, true);
}

static void on_saver_delay(control *c, int ev) {
    int i = ctl_value(c);
    if (ev != EV_CHANGE || i < 0 || i >= SAVER_DELAYS)
        return;
    g_cfg.saver_minutes = (uint8_t)SAVER_DELAY[i];
    set_dirty(c->user, true);
}

static void on_saver_test(control *c, int ev) {
    (void)c;
    if (ev == EV_CLICK)
        saver_start();
}

/* ── Clock tab ─────────────────────────────────────────────────────── */

static int field_num(control *c) {
    const char *t = ctl_text(c);
    int v = 0, n = 0;
    for (; *t; t++) {
        if (*t < '0' || *t > '9')
            return -1;
        v = v * 10 + (*t - '0');
        n++;
    }
    return n ? v : -1;
}

/* weekday under the date fields, or why it isn't a date */
static void clock_weekday(settings_state_t *s) {
    int d = field_num(s->c_day), m = ctl_value(s->c_mon) + 1,
        y = field_num(s->c_year);
    char b[48];
    if (time_date_valid(y, m, d))
        snprintf(b, sizeof(b), "%s, day %d of the year",
                 time_weekday_name(time_weekday(y, m, d)),
                 time_day_of_year(y, m, d));
    else
        snprintf(b, sizeof(b), "Not a date");
    ctl_set_text(s->c_wd, b);
}

/* the live line; with `fields`, the edit fields too */
static void clock_fill(settings_state_t *s, bool fields) {
    time_full_t t = time_rtc_local();
    char b[64];
    snprintf(b, sizeof(b), "Clock: %s %d %s %u, %02u:%02u:%02u",
             time_weekday_name(time_weekday((int)t.year, t.month, t.day)),
             t.day, time_month_name(t.month), t.year, t.hours, t.minutes,
             t.seconds);
    ctl_set_text(s->c_now, b);
    s->clock_shown = (uint32_t)t.hours * 3600 + t.minutes * 60 + t.seconds;
    if (!fields)
        return;
    snprintf(b, sizeof(b), "%u", t.day);
    ctl_set_text(s->c_day, b);
    list_set_sel(s->c_mon, t.month - 1);
    snprintf(b, sizeof(b), "%u", t.year);
    ctl_set_text(s->c_year, b);
    snprintf(b, sizeof(b), "%02u", t.hours);
    ctl_set_text(s->c_h, b);
    snprintf(b, sizeof(b), "%02u", t.minutes);
    ctl_set_text(s->c_m, b);
    snprintf(b, sizeof(b), "%02u", t.seconds);
    ctl_set_text(s->c_s, b);
    clock_weekday(s);
}

static void on_clock_edit(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CHANGE || !s->c_wd)
        return;
    s->clock_edited = true; /* stop following the clock */
    clock_weekday(s);
}

static void on_clock_now(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    s->clock_edited = false;
    clock_fill(s, true);
}

static void on_clock_set(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    int d = field_num(s->c_day), m = ctl_value(s->c_mon) + 1,
        y = field_num(s->c_year), h = field_num(s->c_h), mi = field_num(s->c_m),
        sec = field_num(s->c_s);
    const char *err = NULL;
    if (!time_date_valid(y, m, d))
        err = "That is not a date.";
    else if (y < 1980 || y > 2099)
        err = "The clock chip holds 1980 to 2099.";
    else if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59)
        err = "The time is hours 0-23, minutes and seconds 0-59.";
    if (err) {
        dlg_message(s->win, "Clock", err);
        return;
    }
    time_full_t t = {(uint32_t)y, (uint8_t)m,  (uint8_t)d,
                     (uint8_t)h,  (uint8_t)mi, (uint8_t)sec};
    if (!time_set_local(&t)) {
        dlg_message(s->win, "Clock", "Could not set the clock.");
        return;
    }
    s->clock_edited = false;
    clock_fill(s, true);
    ctl_set_text(s->status, "Clock set");
    ctl_set_colors(s->status, DARK_GRAY, 0xFF);
}

/* once a second: the live line, and the fields unless being edited */
static void settings_frame(void *state) {
    settings_state_t *s = state;
    if (!s->win || !s->c_now || ctl_value(s->tabs) != P_CLOCK)
        return;
    time_full_t t = time_rtc_local();
    uint32_t now = (uint32_t)t.hours * 3600 + t.minutes * 60 + t.seconds;
    if (now != s->clock_shown)
        clock_fill(s, !s->clock_edited);
}

static void build(settings_state_t *s) {
    window *w = s->win;
    s->tabs = ctl_tabs(w, 4, 3, 250,
                       "Display|Sound|System|Network|Saver|Speed|Clock", NULL);

    /* Display: screen mode (kept on the boot disk), wallpaper */
    on_page(s, ctl_label(w, 10, 24, 60, 12, "Screen:", 0), P_WALL);
    s->screen = on_page(s, ctl_combo(w, 76, 24, 130, on_screen), P_WALL);
    list_add(s->screen, "320x200, 256 colours");
    list_add(s->screen, "640x400, 16 colours");
    int vm = video_cfg_get();
    s->screen_saved = vm >= 0 ? vm : (g_video_mode ? VIDEO_640 : VIDEO_320);
    list_set_sel(s->screen, s->screen_saved);
    ctl_enable(s->screen, vm >= 0);
    ctl_set_tooltip(s->screen, vm >= 0 ? "Takes effect when you restart"
                                       : "This boot disk can't store it: "
                                         "rebuild the image");
    on_page(s, ctl_label(w, 10, 40, 60, 12, "Pattern:", 0), P_WALL);
    s->pattern = on_page(s, ctl_combo(w, 76, 40, 110, on_wall), P_WALL);
    for (int i = 0; i < 5; i++)
        list_add(s->pattern, PATTERNS[i]);
    on_page(s, ctl_label(w, 10, 56, 60, 12, "Color A:", 0), P_WALL);
    s->col_a = on_page(s, ctl_combo(w, 76, 56, 110, on_wall), P_WALL);
    on_page(s, ctl_label(w, 10, 72, 60, 12, "Color B:", 0), P_WALL);
    s->col_b = on_page(s, ctl_combo(w, 76, 72, 110, on_wall), P_WALL);
    for (int i = 0; i < 16; i++) {
        list_add(s->col_a, COLORS[i]);
        list_add(s->col_b, COLORS[i]);
    }
    list_set_sel(s->pattern, g_cfg.wallpaper_pattern % 5);
    list_set_sel(s->col_a, g_cfg.wallpaper_main_color & 15);
    list_set_sel(s->col_b, g_cfg.wallpaper_secondary_color & 15);
    on_page(s, ctl_label(w, 10, 88, 60, 8, "Preview:", 0), P_WALL);
    on_page(s, ctl_custom(w, 10, 97, 238, 22, paint_preview, NULL, CS_BORDER),
            P_WALL);
    control *cf = flag(s, P_WALL, 10, 124, "Classic fonts (the old 4x6 type)",
                       &g_cfg.classic_fonts);
    ctl_set_tooltip(cf, "Titles, menus and labels in the original font");

    /* Saver */
    on_page(s, ctl_label(w, 10, 24, 60, 12, "Effect:", 0), P_SAVER);
    control *fx = on_page(s, ctl_combo(w, 76, 24, 110, on_saver_fx), P_SAVER);
    for (int i = 0; i < SAVER_COUNT; i++)
        list_add(fx, SAVER_NAMES[i]);
    list_set_sel(fx, g_cfg.saver_effect < SAVER_COUNT ? g_cfg.saver_effect : 0);
    on_page(s, ctl_label(w, 10, 40, 60, 12, "Start after:", 0), P_SAVER);
    control *dl =
        on_page(s, ctl_combo(w, 76, 40, 110, on_saver_delay), P_SAVER);
    int pick = 0;
    for (int i = 0; i < SAVER_DELAYS; i++) {
        char t[16];
        if (SAVER_DELAY[i])
            snprintf(t, sizeof(t), "%d minute%s", SAVER_DELAY[i],
                     SAVER_DELAY[i] > 1 ? "s" : "");
        else
            strcpy(t, "Never");
        list_add(dl, t);
        if (SAVER_DELAY[i] == g_cfg.saver_minutes)
            pick = i;
    }
    list_set_sel(dl, pick);
    on_page(s, ctl_custom(w, 10, 58, 238, 58, paint_saver, NULL, CS_BORDER),
            P_SAVER);
    control *tb = on_page(
        s, ctl_button(w, 10, 121, 50, 13, "Test", 0, on_saver_test), P_SAVER);
    ctl_set_tooltip(tb, "Move the mouse or press a key to come back");

    /* Sound */
    flag(s, P_SOUND, 10, 24, "Sound enabled", &g_cfg.sound_enabled);
    flag(s, P_SOUND, 10, 37, "Play boot chime", &g_cfg.play_bootchime);
    bool det = sb16_detected();
    on_page(
        s,
        ctl_group(w, 6, 52, 244, 78, det ? sb_model_name() : "Sound Blaster"),
        P_SOUND);
    char sbinfo[64];
    if (det)
        snprintf(sbinfo, sizeof(sbinfo), "Port %X, IRQ %d, DMA %d, DSP %d.%02d",
                 sb_port(), sb_irq(), sb_dma(), sb_dsp_version() >> 8,
                 sb_dsp_version() & 0xFF);
    else
        strcpy(sbinfo, "Not installed (no card at 0x220-0x280)");
    on_page(s, ctl_label(w, 14, 64, 220, 8, sbinfo, 0), P_SOUND);
    on_page(s, ctl_label(w, 14, 80, 50, 11, "Volume:", 0), P_SOUND);
    s->vol = on_page(
        s, ctl_slider(w, 60, 80, 120, 0, 15, g_cfg.sb16_volume, on_volume),
        P_SOUND);
    s->vol_lbl = on_page(s, ctl_label(w, 186, 80, 50, 11, "", 0), P_SOUND);
    ctl_enable(s->vol, det && sb_dsp_version() >= 0x300); /* SB Pro+ mixer */
    char b[16];
    snprintf(b, sizeof(b), "%d / 15", g_cfg.sb16_volume);
    ctl_set_text(s->vol_lbl, b);

    /* System */
    flag(s, P_SYSTEM, 10, 24, "Start in the GUI (else the text shell)",
         &g_cfg.start_in_gui);
    on_page(s, ctl_group(w, 6, 46, 244, 40, "File manager"), P_SYSTEM);
    control *r1 = on_page(s,
                          ctl_radio(w, 14, 58, 220, "One window per folder", 1,
                                    !g_cfg.filef_single_window, on_ffmode),
                          P_SYSTEM);
    control *r2 = on_page(s,
                          ctl_radio(w, 14, 70, 220, "Reuse a single window", 1,
                                    g_cfg.filef_single_window, on_ffmode),
                          P_SYSTEM);
    r1->id = 0;
    r2->id = 1;
    control *np = flag(s, P_SYSTEM, 10, 96,
                       "No app memory protection (restart)", &g_cfg.no_paging);
    control *fe = flag(s, P_SYSTEM, 10, 108, "Emulate the maths chip (restart)",
                       &g_cfg.fpu_emulate);
    ctl_set_tooltip(fe, "Test the FPU emulator on a machine that has an FPU");
    ctl_set_tooltip(np, "Paging keeps apps out of each other's memory");

    /* Network */
    flag(s, P_NET, 10, 24, "Networking enabled", &g_cfg.networking_enabled);
    static const char *lbl[4] = {
        "IP address:", "Gateway:", "Netmask:", "DNS server:"};
    uint8_t *src[4] = {g_cfg.ip, g_cfg.gateway, g_cfg.netmask,
                       g_cfg.dns_server};
    for (int i = 0; i < 4; i++) {
        char t[20];
        snprintf(t, sizeof(t), "%d.%d.%d.%d", src[i][0], src[i][1], src[i][2],
                 src[i][3]);
        on_page(s, ctl_label(w, 10, 42 + i * 16, 60, 12, lbl[i], 0), P_NET);
        s->ip[i] = on_page(
            s, ctl_edit(w, 76, 42 + i * 16, 90, 12, t, 15, ES_DECIMAL, on_edit),
            P_NET);
    }
    on_page(s,
            ctl_label(w, 10, 110, 240, 8,
                      "QEMU user networking: DNS 10.0.2.3 or 8.8.8.8", 0),
            P_NET);

    /* Speed: what a slow machine can do without (each flag is stored as
     * "off", so 0 = all on) */
    for (int i = 0; i < PERF_FLAGS; i++) {
        s->perf[i] = on_page(s,
                             ctl_check(w, 10, 22 + i * 13, 240, PERF[i].text,
                                       *PERF[i].off == 0, on_perf),
                             P_PERF);
        s->perf[i]->id = i;
        ctl_set_tooltip(s->perf[i], PERF[i].tip);
    }
    on_page(s, ctl_label(w, 10, 102, 60, 12, "Frame rate:", 0), P_PERF);
    s->fps = on_page(s, ctl_combo(w, 76, 102, 80, on_fps), P_PERF);
    list_add(s->fps, "60 a second");
    list_add(s->fps, "30 a second");
    list_add(s->fps, "20 a second");
    list_set_sel(s->fps, g_cfg.fps_cap < 3 ? g_cfg.fps_cap : 0);
    ctl_set_tooltip(s->fps, "Lower leaves more time for programs "
                            "working in the background");
    control *lo = on_page(
        s, ctl_button(w, 10, 121, 90, 13, "Low-end machine", 0, on_preset),
        P_PERF);
    lo->id = 1;
    ctl_set_tooltip(lo, "Everything light: for a 386");
    control *hi =
        on_page(s, ctl_button(w, 106, 121, 70, 13, "All effects", 0, on_preset),
                P_PERF);
    hi->id = 0;

    /* Clock: date, time, time zone. Set Clock writes the clock chip at
     * once; the time zone is a setting (Save). */
    on_page(s, ctl_label(w, 10, 24, 60, 12, "Date:", 0), P_CLOCK);
    s->c_day = on_page(
        s, ctl_edit(w, 76, 24, 26, 12, "", 2, ES_DECIMAL, on_clock_edit),
        P_CLOCK);
    s->c_mon = on_page(s, ctl_combo(w, 106, 24, 84, on_clock_edit), P_CLOCK);
    for (int m = 1; m <= 12; m++)
        list_add(s->c_mon, time_month_name(m));
    s->c_year = on_page(
        s, ctl_edit(w, 194, 24, 36, 12, "", 4, ES_DECIMAL, on_clock_edit),
        P_CLOCK);
    s->c_wd = on_page(s, ctl_label(w, 76, 38, 170, 9, "", 0), P_CLOCK);
    on_page(s, ctl_label(w, 10, 50, 60, 12, "Time:", 0), P_CLOCK);
    s->c_h = on_page(
        s, ctl_edit(w, 76, 50, 26, 12, "", 2, ES_DECIMAL, on_clock_edit),
        P_CLOCK);
    on_page(s, ctl_label(w, 103, 50, 4, 12, ":", 0), P_CLOCK);
    s->c_m = on_page(
        s, ctl_edit(w, 108, 50, 26, 12, "", 2, ES_DECIMAL, on_clock_edit),
        P_CLOCK);
    on_page(s, ctl_label(w, 135, 50, 4, 12, ":", 0), P_CLOCK);
    s->c_s = on_page(
        s, ctl_edit(w, 140, 50, 26, 12, "", 2, ES_DECIMAL, on_clock_edit),
        P_CLOCK);
    on_page(s, ctl_label(w, 172, 50, 70, 12, "24-hour", 0), P_CLOCK);
    control *set =
        on_page(s, ctl_button(w, 76, 68, 60, 13, "Set Clock", 0, on_clock_set),
                P_CLOCK);
    ctl_set_tooltip(set, "Write this date and time to the clock chip now");
    control *now = on_page(
        s, ctl_button(w, 142, 68, 50, 13, "Now", 0, on_clock_now), P_CLOCK);
    ctl_set_tooltip(now, "Show the clock's time again");
    on_page(s, ctl_label(w, 10, 92, 60, 12, "Time zone:", 0), P_CLOCK);
    control *tz = on_page(s, ctl_combo(w, 76, 92, 80, on_tz), P_CLOCK);
    for (int h = -12; h <= 14; h++) {
        char t[12];
        snprintf(t, sizeof(t), "UTC%+d", h);
        list_add(tz, t);
    }
    list_set_sel(tz, (int)(int8_t)g_cfg.timezone_offset + 12);
    s->c_now = on_page(s, ctl_label(w, 10, 112, 240, 9, "", 0), P_CLOCK);
    clock_fill(s, true);

    /* bottom bar */
    s->status = ctl_label(w, 6, 140, 180, 13, "", 0);
    s->save = ctl_button(w, 200, 140, 50, 13, "Save", 1, on_save);
    ctl_style_add(s->save, CS_DEFAULT);
    set_dirty(s, false);
}

static void on_close(void) { os_quit_app_by_desc(&settings_app); }

static void settings_init(void *state) {
    settings_state_t *s = state;
    if (g_cfg.sb16_volume == 0)
        g_cfg.sb16_volume = 8;
    s->win = app_window(s, "Settings", 30, 20, 260, 172, DARK_GRAY, 0);
    if (!s->win)
        return;
    s->win->lazy_custom =
        true; /* previews: handlers; the saver one asks itself */
    build(s);
    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "Close", on_close);
}

app_descriptor settings_app = {
    .name = "Settings",
    .state_size = sizeof(settings_state_t),
    .init = settings_init,
    .on_frame = settings_frame,
    .single_instance = true,
};
