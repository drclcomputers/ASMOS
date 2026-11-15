/* Settings - edits g_cfg; wallpaper/sound changes apply at once, Save writes
 * ASMOS.CFG. */
#include "config/runtime_config.h"
#include "drivers/sb16.h"
#include "network/gopher.h" /* parse_ip_str */
#include "os/api.h"

enum { P_WALL, P_SOUND, P_SYSTEM, P_NET };

typedef struct {
    window *win; /* must stay first */
    control *tabs, *status, *save;
    control *pattern, *col_a, *col_b;
    control *vol, *vol_lbl;
    control *ip[4]; /* ip, gateway, netmask, dns */
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

static void on_tz(control *c, int ev) {
    if (ev != EV_CHANGE)
        return;
    g_cfg.timezone_offset = (uint8_t)(int8_t)(ctl_value(c) - 12);
    set_dirty(c->user, true);
}

static void on_ffmode(control *c, int ev) {
    if (ev != EV_CHANGE)
        return;
    g_cfg.filef_single_window = (uint8_t)c->id;
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
    if (cfg_save()) {
        set_dirty(s, false);
        ctl_set_text(s->status, "Saved. Network: applies on restart.");
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

static void build(settings_state_t *s) {
    window *w = s->win;
    s->tabs = ctl_tabs(w, 4, 3, 250, "Wallpaper|Sound|System|Network", NULL);

    /* Wallpaper */
    on_page(s, ctl_label(w, 10, 24, 60, 12, "Pattern:", 0), P_WALL);
    s->pattern = on_page(s, ctl_combo(w, 76, 24, 110, on_wall), P_WALL);
    for (int i = 0; i < 5; i++)
        list_add(s->pattern, PATTERNS[i]);
    on_page(s, ctl_label(w, 10, 40, 60, 12, "Color A:", 0), P_WALL);
    s->col_a = on_page(s, ctl_combo(w, 76, 40, 110, on_wall), P_WALL);
    on_page(s, ctl_label(w, 10, 56, 60, 12, "Color B:", 0), P_WALL);
    s->col_b = on_page(s, ctl_combo(w, 76, 56, 110, on_wall), P_WALL);
    for (int i = 0; i < 16; i++) {
        list_add(s->col_a, COLORS[i]);
        list_add(s->col_b, COLORS[i]);
    }
    list_set_sel(s->pattern, g_cfg.wallpaper_pattern % 5);
    list_set_sel(s->col_a, g_cfg.wallpaper_main_color & 15);
    list_set_sel(s->col_b, g_cfg.wallpaper_secondary_color & 15);
    on_page(s, ctl_label(w, 10, 76, 60, 8, "Preview:", 0), P_WALL);
    on_page(s, ctl_custom(w, 10, 86, 238, 44, paint_preview, NULL, CS_BORDER),
            P_WALL);

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
    flag(s, P_SYSTEM, 10, 24, "Reduce motion (no window animations)",
         &g_cfg.reduce_motion);
    flag(s, P_SYSTEM, 10, 37, "Start in the GUI (else the text shell)",
         &g_cfg.start_in_gui);
    on_page(s, ctl_label(w, 10, 54, 60, 12, "Time zone:", 0), P_SYSTEM);
    control *tz = on_page(s, ctl_combo(w, 76, 54, 80, on_tz), P_SYSTEM);
    for (int h = -12; h <= 14; h++) {
        char t[12];
        snprintf(t, sizeof(t), "UTC%+d", h);
        list_add(tz, t);
    }
    list_set_sel(tz, (int)(int8_t)g_cfg.timezone_offset + 12);
    on_page(s, ctl_group(w, 6, 72, 244, 40, "File manager"), P_SYSTEM);
    control *r1 = on_page(s,
                          ctl_radio(w, 14, 84, 220, "One window per folder", 1,
                                    !g_cfg.filef_single_window, on_ffmode),
                          P_SYSTEM);
    control *r2 = on_page(s,
                          ctl_radio(w, 14, 96, 220, "Reuse a single window", 1,
                                    g_cfg.filef_single_window, on_ffmode),
                          P_SYSTEM);
    r1->id = 0;
    r2->id = 1;

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
    build(s);
    menu *f = window_add_menu(s->win, "File");
    menu_add_item(f, "Close", on_close);
}

app_descriptor settings_app = {
    .name = "Settings",
    .state_size = sizeof(settings_state_t),
    .init = settings_init,
    .single_instance = true,
};
