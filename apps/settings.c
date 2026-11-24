// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "config/runtime_config.h"
#include "config/video_cfg.h"
#include "drivers/joystick.h"
#include "drivers/lpt.h"
#include "drivers/ne2000.h"
#include "drivers/sb16.h"
#include "drivers/sermouse.h"
#include "lib/time.h"
#include "network/dhcp.h"
#include "network/dns.h"
#include "network/gopher.h"
#include "network/net.h"
#include "os/api.h"
#include "os/print.h"
#include "ui/saver.h"
#include "ui/uifont.h"

/* tabs; Screen Saver and Speed open from Display */
enum { P_WALL, P_SOUND, P_CLOCK, P_NET, P_PRINT, P_DEVICES, P_SYSTEM };

#define PERF_FLAGS 5

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
    window *sub;          /* Screen Saver or Speed, while open */
    control *p_type, *p_port, *p_status;
    control *dev_list; /* Devices tab */
    /* the pattern editor, while open: its copy of the pattern, the two
     * colours (left button / right button) */
    uint8_t pat[64];
    uint8_t pen, back;
    int pat_lx, pat_ly; /* last cell painted while dragging */
    uint32_t p_rev, p_tick;
    bool dirty;
    /* what only a restart applies, as last saved: Save says "restart" only
     * when one of these changed */
    uint8_t boot_sig[24];
    control *dhcp, *net_state; /* Network: DHCP */
    int net_shown; /* the DHCP state last shown (+ 1; 0 = not yet) */
} settings_state_t;

/* the settings that take effect at the next start */
static void boot_settings(uint8_t *out) {
    uint8_t *o = out;
    *o++ = g_cfg.ui_zoom2;
    *o++ = g_cfg.no_paging;
    *o++ = g_cfg.fpu_emulate;
    *o++ = g_cfg.networking_enabled;
    *o++ = g_cfg.start_in_gui;
    for (int i = 0; i < 16; i++) /* (the addresses apply at once now) */
        *o++ = 0;
    *o++ = 0;
    *o++ = 0;
    *o++ = 0;
}

app_descriptor settings_app;

static const char *COLORS[16] = {
    "Black",     "Blue",          "Green",       "Cyan",
    "Red",       "Magenta",       "Brown",       "Light Gray",
    "Dark Gray", "Light Blue",    "Light Green", "Light Cyan",
    "Light Red", "Light Magenta", "Yellow",      "White"};
#define NPATTERNS 6
static const char *PATTERNS[NPATTERNS] = {
    "Solid", "Checkerboard", "Stripes", "Vertical stripes", "Dots", "Custom"};

static void set_dirty(settings_state_t *s, bool d) {
    s->dirty = d;
    ctl_set_text(s->status, d ? "Unsaved changes" : "All saved");
    ctl_set_colors(s->status, d ? RED : DARK_GRAY, 0xFF);
}

// wallpaper preview

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
    case WALLPAPER_CUSTOM: { /* as it is on the desktop */
        static uint8_t row[640];
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w && xx < 640; xx++)
                row[xx] = cfg_wall_px(xx, yy);
            draw_pixels(x, y + yy, w, 1, row, w);
        }
        break;
    }
    default:
        fill_rect(x, y, w, h, a);
    }
}

// handlers

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

// Network tab: DHCP, or the addresses typed in

/* the fields: typed in, or (DHCP) what it got, greyed */
static void net_fields(settings_state_t *s) {
    bool dhcp = ctl_value(s->dhcp);
    dhcp_info_t d;
    dhcp_info(&d);
    const uint8_t *src[4] = {g_cfg.ip, g_cfg.gateway, g_cfg.netmask,
                             g_cfg.dns_server};
    const uint8_t *got[4] = {d.ip, d.gateway, d.netmask, d.dns};
    for (int i = 0; i < 4; i++) {
        ctl_enable(s->ip[i], !dhcp);
        if (!dhcp && s->net_shown)
            continue; /* (as typed) */
        const uint8_t *a = dhcp ? got[i] : src[i];
        char t[20];
        snprintf(t, sizeof(t), "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
        ctl_set_text(s->ip[i], dhcp && d.state < DHCP_BOUND ? "" : t);
    }
}

/* "Got 10.0.2.15 from 10.0.2.2, 23 h left" */
static void net_status(settings_state_t *s) {
    char t[64];
    dhcp_info_t d;
    dhcp_info(&d);
    if (!g_cfg.networking_enabled || !ne2000_detected())
        snprintf(t, sizeof(t), "%s",
                 ne2000_detected() ? "Networking is off." : "No network card.");
    else if (d.state == DHCP_OFF)
        snprintf(t, sizeof(t), "Fixed address.");
    else if (d.state < DHCP_BOUND)
        snprintf(t, sizeof(t), "Asking the network for an address...");
    else if (d.lease_s == 0xFFFFFFFFu)
        snprintf(t, sizeof(t), "From %d.%d.%d.%d, for good.", d.server[0],
                 d.server[1], d.server[2], d.server[3]);
    else
        snprintf(t, sizeof(t), "From %d.%d.%d.%d, %u %s left.", d.server[0],
                 d.server[1], d.server[2], d.server[3],
                 d.lease_left_s >= 7200 ? d.lease_left_s / 3600
                                        : d.lease_left_s / 60,
                 d.lease_left_s >= 7200 ? "h" : "min");
    ctl_set_text(s->net_state, t);
}

static void on_dhcp(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CHANGE)
        return;
    set_dirty(s, true);
    s->net_shown = 0; /* fill the fields again */
    net_fields(s);
    s->net_shown = 1;
}

static void on_renew(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CLICK || !g_cfg.net_dhcp || !ne2000_detected())
        return;
    dhcp_start();
    s->net_shown = 0;
}

/* Save: the addresses apply at once (DHCP starts or stops) */
static void net_apply(void) {
    if (!g_cfg.networking_enabled || !ne2000_detected())
        return;
    if (g_cfg.net_dhcp) {
        if (dhcp_state() == DHCP_OFF)
            dhcp_start();
        return;
    }
    dhcp_stop(true);
    net_set_ip(g_cfg.ip, g_cfg.gateway, g_cfg.netmask);
    dns_set_server(g_cfg.dns_server);
    arp_announce();
}

static void on_save(control *c, int ev) {
    settings_state_t *s = c->user;
    (void)ev;
    static const char *names[4] = {"IP address", "Gateway", "Netmask",
                                   "DNS server"};
    uint8_t *dst[4] = {g_cfg.ip, g_cfg.gateway, g_cfg.netmask,
                       g_cfg.dns_server};
    uint8_t tmp[4][4];
    bool dhcp = ctl_value(s->dhcp);
    for (int i = 0; i < 4 && !dhcp; i++)
        if (!parse_ip_str(ctl_text(s->ip[i]), tmp[i])) {
            char m[64];
            snprintf(m, sizeof(m), "%s must look like 10.0.2.15.", names[i]);
            ctl_set_value(s->tabs, P_NET);
            ctl_focus(s->ip[i]);
            dlg_message(s->win, "Settings", m);
            return;
        }
    for (int i = 0; i < 4 && !dhcp; i++)
        memcpy(dst[i], tmp[i], 4);
    g_cfg.net_dhcp = dhcp;
    net_apply();
    s->net_shown = 0;
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
        uint8_t now[24];
        boot_settings(now);
        bool restart = screen_changed || memcmp(now, s->boot_sig, 24) != 0;
        memcpy(s->boot_sig, now, 24);
        ctl_set_text(s->status,
                     restart ? "Saved. Restart to apply." : "Saved.");
    } else {
        dlg_message(s->win, "Settings", "Could not write ASMOS.CFG.");
    }
}

// building the pages

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
};

static void on_perf(control *c, int ev) {
    if (ev != EV_CHANGE || c->id < 0 || c->id >= PERF_FLAGS)
        return;
    *PERF[c->id].off = ctl_value(c) ? 0 : 1;
    set_dirty(c->user, true);
}

/* the characters 128-255: at once, everywhere */
static void on_codepage(control *c, int ev) {
    int i = ctl_value(c);
    if (ev != EV_CHANGE || i < 0 || i >= CP_COUNT)
        return;
    g_cfg.codepage = (uint8_t)i;
    g_codepage = i;
    wm_invalidate_all();
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
        if (s->perf[i])
            ctl_set_value(s->perf[i], *PERF[i].off == 0);
    if (s->fps)
        list_set_sel(s->fps, g_cfg.fps_cap);
    set_dirty(s, true);
}

static const int SAVER_DELAY[] = {0, 1, 2, 3, 5, 10, 15, 30};
#define SAVER_DELAYS ((int)(sizeof(SAVER_DELAY) / sizeof(SAVER_DELAY[0])))

static void paint_saver(control *c, int x, int y, int w, int h) {
    if (g_cfg.reduce_motion) { /* Speed > Window animations off: still */
        static const char *const L[2] = {"Preview is turned off",
                                         "to improve performance."};
        fill_rect(x, y, w, h, BLACK);
        int f = UI_FONT_SMALL, lh = font_height(f) + 3;
        int ty = y + (h - 2 * lh) / 2;
        for (int i = 0; i < 2; i++)
            draw_string(x + (w - text_width(L[i], f)) / 2, ty + i * lh, L[i],
                        LIGHT_GRAY, f);
        return; /* drawn once: no animation frames */
    }
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

// Screen Saver and Speed: small windows over Settings

static void close_sub(settings_state_t *s) {
    if (!s->sub)
        return;
    window *w = s->sub;
    s->sub = NULL;
    for (int i = 0; i < PERF_FLAGS; i++)
        s->perf[i] = NULL;
    s->fps = NULL;
    if (s->win && s->win->modal_child == w)
        s->win->modal_child = NULL;
    wm_unregister(w);
    if (s->win)
        wm_focus(s->win);
}

static bool on_sub_close(window *w) {
    close_sub(w->userdata);
    return true;
}

static void on_sub_ok(control *c, int ev) {
    if (ev == EV_CLICK)
        close_sub(c->user);
}

static window *open_sub(settings_state_t *s, const char *title, int w, int h) {
    if (s->sub)
        close_sub(s);
    window *sw = app_window(s, title, s->win->x + 16, s->win->y + 16, w, h,
                            DARK_GRAY, 0);
    if (!sw)
        return NULL;
    sw->on_close = on_sub_close;
    sw->lazy_custom = true;
    sw->modal_owner = s->win; /* Settings waits until this one closes */
    s->win->modal_child = sw;
    s->sub = sw;
    return sw;
}

static void open_saver(settings_state_t *s) {
    window *w = open_sub(s, "Screen Saver", 260, 158);
    if (!w)
        return;
    ctl_label(w, 10, 6, 60, 12, "Effect:", 0);
    control *fx = ctl_combo(w, 76, 6, 110, on_saver_fx);
    for (int i = 0; i < SAVER_COUNT; i++)
        list_add(fx, SAVER_NAMES[i]);
    list_set_sel(fx, g_cfg.saver_effect < SAVER_COUNT ? g_cfg.saver_effect : 0);
    ctl_label(w, 10, 22, 60, 12, "Start after:", 0);
    control *dl = ctl_combo(w, 76, 22, 110, on_saver_delay);
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
    ctl_custom(w, 10, 40, 238, 76, paint_saver, NULL, CS_BORDER);
    control *tb = ctl_button(w, 10, 122, 50, 13, "Test", 0, on_saver_test);
    ctl_set_tooltip(tb, "Move the mouse or press a key to come back");
    control *ok = ctl_button(w, 198, 122, 50, 13, "OK", 1, on_sub_ok);
    ctl_style_add(ok, CS_DEFAULT | CS_CANCEL);
    ctl_focus(ok);
}

/* each flag is stored as "off", so 0 = all on */
static void open_speed(settings_state_t *s) {
    window *w = open_sub(s, "Speed", 260, 158);
    if (!w)
        return;
    for (int i = 0; i < PERF_FLAGS; i++) {
        s->perf[i] = ctl_check(w, 10, 6 + i * 13, 240, PERF[i].text,
                               *PERF[i].off == 0, on_perf);
        s->perf[i]->id = i;
        ctl_set_tooltip(s->perf[i], PERF[i].tip);
    }
    ctl_label(w, 10, 86, 60, 12, "Frame rate:", 0);
    s->fps = ctl_combo(w, 76, 86, 80, on_fps);
    list_add(s->fps, "60 a second");
    list_add(s->fps, "30 a second");
    list_add(s->fps, "20 a second");
    list_set_sel(s->fps, g_cfg.fps_cap < 3 ? g_cfg.fps_cap : 0);
    ctl_set_tooltip(s->fps, "Lower leaves more time for programs "
                            "working in the background");
    control *lo =
        ctl_button(w, 10, 104, 90, 13, "Low-end machine", 0, on_preset);
    lo->id = 1;
    ctl_set_tooltip(lo, "Everything light: for a 386");
    control *hi = ctl_button(w, 106, 104, 70, 13, "All effects", 0, on_preset);
    hi->id = 0;
    ctl_label(w, 10, 124, 180, 9, "Changes apply now; Save keeps them.", 0);
    control *ok = ctl_button(w, 198, 122, 50, 13, "OK", 1, on_sub_ok);
    ctl_style_add(ok, CS_DEFAULT | CS_CANCEL);
    ctl_focus(ok);
}

// desktop pattern editor

#define PAT_CELL 11 /* a pixel of the pattern, in the grid */

static void paint_pat_grid(control *c, int x, int y, int w, int h) {
    settings_state_t *s = c->user;
    fill_rect(x, y, w, h, DARK_GRAY); /* the lines between cells */
    for (int cy = 0; cy < 8; cy++)
        for (int cx = 0; cx < 8; cx++)
            fill_rect(x + 1 + cx * PAT_CELL, y + 1 + cy * PAT_CELL,
                      PAT_CELL - 1, PAT_CELL - 1, s->pat[cy * 8 + cx]);
}

static void paint_pat_preview(control *c, int x, int y, int w, int h) {
    settings_state_t *s = c->user;
    static uint8_t row[640];
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w && xx < 640; xx++)
            row[xx] = s->pat[(yy & 7) * 8 + (xx & 7)];
        draw_pixels(x, y + yy, w, 1, row, w);
    }
}

static void paint_pat_palette(control *c, int x, int y, int w, int h) {
    settings_state_t *s = c->user;
    /* the two colours: left button's over right button's (as ASMDraw) */
    fill_rect(x + 6, y + 3, 10, h - 6, s->back);
    draw_rect(x + 6, y + 3, 10, h - 6, BLACK);
    fill_rect(x + 1, y + 1, 10, h - 6, s->pen);
    draw_rect(x + 1, y + 1, 10, h - 6, BLACK);
    int cw = (w - 20) / 16;
    for (int i = 0; i < 16; i++) {
        fill_rect(x + 20 + i * cw, y + 1, cw - 1, h - 2, (uint8_t)i);
        draw_rect(x + 20 + i * cw, y + 1, cw - 1, h - 2, BLACK);
    }
}

static void on_pat_palette(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_MOUSEDOWN && ev != EV_DBLCLICK && ev != EV_MENU)
        return;
    int cw = (c->aw - 20) / 16;
    int i = cw > 0 && c->mx >= 20 ? (c->mx - 20) / cw : -1;
    if (i < 0 || i > 15)
        return;
    if (ev == EV_MENU || kb.shift)
        s->back = (uint8_t)i;
    else
        s->pen = (uint8_t)i;
}

static void on_pat_grid(control *c, int ev) {
    settings_state_t *s = c->user;
    bool right = ev == EV_MENU || kb.shift;
    if (ev != EV_MOUSEDOWN && ev != EV_DBLCLICK && ev != EV_MOUSEMOVE &&
        ev != EV_MENU)
        return;
    int cx = (c->mx - 1) / PAT_CELL, cy = (c->my - 1) / PAT_CELL;
    if (c->mx < 1)
        cx = -1;
    if (c->my < 1)
        cy = -1;
    if (ev == EV_MOUSEMOVE) { /* every cell between: a fast drag skips */
        cx = cx < 0 ? 0 : cx > 7 ? 7 : cx;
        cy = cy < 0 ? 0 : cy > 7 ? 7 : cy;
        int x = s->pat_lx, y = s->pat_ly;
        int dx = cx > x ? cx - x : x - cx, dy = cy > y ? cy - y : y - cy;
        int sx = cx > x ? 1 : -1, sy = cy > y ? 1 : -1, err = dx - dy;
        for (;;) {
            s->pat[y * 8 + x] = right ? s->back : s->pen;
            if (x == cx && y == cy)
                break;
            int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x += sx;
            }
            if (e2 < dx) {
                err += dx;
                y += sy;
            }
        }
    } else {
        if (cx < 0 || cy < 0 || cx > 7 || cy > 7)
            return;
        s->pat[cy * 8 + cx] = right ? s->back : s->pen;
    }
    s->pat_lx = cx;
    s->pat_ly = cy;
}

static void on_pat_btn(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    switch (c->id) {
    case 0: /* Fill */
        memset(s->pat, s->back, sizeof(s->pat));
        break;
    case 1: { /* Invert: the two colours swap places */
        for (int i = 0; i < 64; i++)
            s->pat[i] = s->pat[i] == s->pen    ? s->back
                        : s->pat[i] == s->back ? s->pen
                                               : s->pat[i];
        break;
    }
    case 2: { /* the default diamond */
        uint8_t d[32];
        cfg_default_pattern(d);
        for (int i = 0; i < 64; i++)
            s->pat[i] = (uint8_t)(i & 1 ? d[i / 2] & 15 : d[i / 2] >> 4);
        break;
    }
    case 3:
    case 4:
    case 5:
    case 6: { /* move the whole pattern a pixel (it wraps round) */
        uint8_t t[64];
        int a = c->id - 3;
        int dx = a == 0 ? 1 : a == 1 ? 7 : 0;
        int dy = a == 2 ? 1 : a == 3 ? 7 : 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                t[((y + dy) & 7) * 8 + ((x + dx) & 7)] = s->pat[y * 8 + x];
        memcpy(s->pat, t, 64);
        break;
    }
    case 10: /* OK: the desktop gets it */
        for (int i = 0; i < 32; i++)
            g_cfg.wall_custom[i] =
                (uint8_t)(s->pat[i * 2] << 4 | s->pat[i * 2 + 1]);
        g_cfg.wallpaper_pattern = WALLPAPER_CUSTOM;
        list_set_sel(s->pattern, WALLPAPER_CUSTOM);
        set_dirty(s, true);
        close_sub(s);
        break;
    case 11: /* Cancel */
        close_sub(s);
        break;
    }
}

static void open_pattern(settings_state_t *s) {
    window *w = open_sub(s, "Desktop Pattern", 260, 158);
    if (!w)
        return;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            s->pat[y * 8 + x] = cfg_wall_px(x, y);
    /* start with the pattern's two most used colours */
    int cnt[16] = {0};
    for (int i = 0; i < 64; i++)
        cnt[s->pat[i] & 15]++;
    int a = 0, b = -1;
    for (int i = 1; i < 16; i++)
        if (cnt[i] > cnt[a])
            a = i;
    for (int i = 0; i < 16; i++)
        if (i != a && cnt[i] && (b < 0 || cnt[i] > cnt[b]))
            b = i;
    s->back = (uint8_t)a;
    s->pen = (uint8_t)(b >= 0 ? b : (a == BLACK ? WHITE : BLACK));

    control *g = ctl_custom(w, 10, 6, 8 * PAT_CELL + 1, 8 * PAT_CELL + 1,
                            paint_pat_grid, on_pat_grid, 0);
    ctl_set_tooltip(g, "Left button: front colour; right button (or "
                       "Shift): back colour");
    ctl_label(w, 108, 6, 140, 9, "On the desktop:", 0);
    ctl_custom(w, 108, 16, 140, 44, paint_pat_preview, NULL, CS_BORDER);
    static const char *const arrows[4] = {">", "<", "v", "^"};
    static const char *const atips[4] = {"Move right", "Move left", "Move down",
                                         "Move up"};
    for (int i = 0; i < 4; i++) {
        control *m =
            ctl_button(w, 108 + i * 18, 66, 16, 13, arrows[i], 0, on_pat_btn);
        m->id = 3 + i;
        ctl_set_tooltip(m, atips[i]);
    }
    static const char *const names[3] = {"Fill", "Swap", "Reset"};
    static const char *const tips[3] = {"All of it in the back colour",
                                        "Front and back colours change places",
                                        "Back to the diamond it came with"};
    for (int i = 0; i < 3; i++) {
        control *b2 = ctl_button(w, 108 + (i % 3) * 47, 84, 45, 13, names[i], 0,
                                 on_pat_btn);
        b2->id = i;
        ctl_set_tooltip(b2, tips[i]);
    }
    control *pal =
        ctl_custom(w, 10, 102, 238, 14, paint_pat_palette, on_pat_palette, 0);
    ctl_set_tooltip(pal, "Click: front colour, right-click: back colour");
    control *ok = ctl_button(w, 144, 122, 50, 13, "OK", 1, on_pat_btn);
    ok->id = 10;
    control *ca = ctl_button(w, 198, 122, 50, 13, "Cancel", 2, on_pat_btn);
    ca->id = 11;
    ctl_style_add(ok, CS_DEFAULT);
    ctl_style_add(ca, CS_CANCEL);
    ctl_focus(ok);
}

static void on_open_sub(control *c, int ev) {
    if (ev != EV_CLICK)
        return;
    if (c->id == 0)
        open_saver(c->user);
    else if (c->id == 1)
        open_speed(c->user);
    else
        open_pattern(c->user);
}

// Printers tab

static void prn_status(settings_state_t *s) {
    char b[80];
    int n = print_job_count();
    if (n)
        snprintf(b, sizeof(b), "Status: %s, %d job%s waiting",
                 print_status_text(), n, n == 1 ? "" : "s");
    else
        snprintf(b, sizeof(b), "Status: %s", print_status_text());
    ctl_set_text(s->p_status, b);
    s->p_rev = print_queue_rev();
}

static void on_prn_type(control *c, int ev) {
    if (ev != EV_CHANGE)
        return;
    g_cfg.printer_type = (uint8_t)ctl_value(c);
    set_dirty(c->user, true);
}

static void on_prn_port(control *c, int ev) {
    if (ev != EV_CHANGE)
        return;
    g_cfg.printer_port = (uint8_t)ctl_value(c);
    set_dirty(c->user, true);
    prn_status(c->user);
}

static void on_prn_ff(control *c, int ev) {
    if (ev != EV_CHANGE)
        return;
    g_cfg.printer_no_ff = ctl_value(c) ? 0 : 1;
    set_dirty(c->user, true);
}

extern app_descriptor printq_app;
static void on_prn_queue(control *c, int ev) {
    (void)c;
    if (ev == EV_CLICK)
        os_launch_app(&printq_app);
}

static void on_prn_test(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    if (!print_available()) {
        dlg_message(s->win, "Test Page",
                    "There is no printer port here (LPT1-3).");
        return;
    }
    static char t[1400];
    int n = 0;
    time_full_t now = time_rtc_local();
    n += snprintf(t + n, sizeof(t) - n,
                  "ASMOS %s printer test page\n"
                  "%d %s %u, %02u:%02u\n\n"
                  "Printer type: %s\nPort: %s\n\n"
                  "Every printable character:\n",
                  VER, now.day, time_month_name(now.month), now.year, now.hours,
                  now.minutes, PRN_TYPE_NAMES[g_cfg.printer_type % PRN_TYPES],
                  print_port_name());
    for (int ch = 32; ch < 127; ch++) {
        t[n++] = (char)ch;
        if ((ch - 31) % 32 == 0)
            t[n++] = '\n';
    }
    n += snprintf(t + n, sizeof(t) - n,
                  "\n\nColumns:\n"
                  "1234567890123456789012345678901234567890"
                  "1234567890123456789012345678901234567890\n"
                  "\tTab\tstops\tevery\teight\tcolumns\n\n"
                  "If this page looks right, the printer works.\n");
    if (print_submit_text("Test page", t, (uint32_t)n, 1, 0, 0) < 0)
        dlg_message(s->win, "Test Page", "The print queue is full.");
    prn_status(s);
}

// Devices tab: what is plugged in. A double-click sets one up.

extern app_descriptor joyconf_app;
enum { DEV_NONE, DEV_JOY, DEV_SOUND, DEV_NET, DEV_PRINT };

static void dev_add(settings_state_t *s, int action, const char *fmt, ...) {
    char t[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    list_add_data(s->dev_list, t, (uint32_t)action);
}

static void dev_fill(settings_state_t *s) {
    list_clear(s->dev_list);
    list_add(s->dev_list, "Device\tConnection\tDetails");
    dev_add(s, DEV_NONE, "Keyboard\tPS/2 port\tConnected");
    mouse_kind_t mk = mouse_kinds();
    if (mk & MOUSE_PS2)
        dev_add(s, DEV_NONE, "Mouse\tPS/2 port\tConnected");
    if (mk & MOUSE_SERIAL)
        dev_add(s, DEV_NONE, "Mouse\tCOM%d\tSerial", sermouse_port());
    if (joy.present) {
        int n = 0;
        for (int i = 0; i < JOY_AXES; i++)
            n += (joy.axes >> i) & 1;
        dev_add(s, DEV_JOY, "Joystick\tGame port %Xh\t%d axes%s", JOY_PORT, n,
                g_cfg.joy.calibrated ? "" : ", uncalibrated");
    } else {
        dev_add(s, DEV_JOY, "Joystick\tGame port %Xh\tNot found", JOY_PORT);
    }
    for (int i = 0; i < 4; i++)
        if (com_base(i) && sermouse_port() != i + 1)
            dev_add(s, DEV_NONE, "Serial port\tCOM%d %Xh\tFree", i + 1,
                    com_base(i));
    for (int i = 0; i < LPT_MAX; i++)
        if (lpt_base(i))
            dev_add(s, DEV_PRINT, "Printer port\tLPT%d %Xh\t%s", i + 1,
                    lpt_base(i), lpt_status_text(lpt_status(i)));
    if (sb16_detected())
        dev_add(s, DEV_SOUND, "%s\t%Xh IRQ %d\tDMA %d", sb_model_name(),
                sb_port(), sb_irq(), sb_dma());
    if (ne2000_detected())
        dev_add(s, DEV_NET, "Network card\t%s %Xh\tNE2000", ne2000_bus(),
                ne2000_io_base());
    list_set_sel(s->dev_list, 1);
}

static void on_dev(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_SUBMIT || list_sel(c) < 1)
        return;
    switch (list_data(c, list_sel(c))) {
    case DEV_JOY:
        os_launch_app(&joyconf_app);
        break;
    case DEV_SOUND:
        ctl_set_value(s->tabs, P_SOUND);
        break;
    case DEV_NET:
        ctl_set_value(s->tabs, P_NET);
        break;
    case DEV_PRINT:
        ctl_set_value(s->tabs, P_PRINT);
        break;
    }
}

static void on_dev_refresh(control *c, int ev) {
    settings_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    joy_detect(); /* the game port has no plug-in signal */
    dev_fill(s);
}

// Clock tab

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
    if (!s->win || !s->c_now)
        return;
    if (ctl_value(s->tabs) == P_PRINT && s->p_status &&
        (print_queue_rev() != s->p_rev || pit_ticks - s->p_tick > 1000)) {
        s->p_tick = pit_ticks; /* the printer's own state: once a second */
        prn_status(s);
    }
    if (ctl_value(s->tabs) == P_NET && s->net_state &&
        (pit_ticks - s->p_tick > 1000 ||
         s->net_shown != (int)dhcp_state() + 1)) {
        s->p_tick = pit_ticks; /* once a second, or when DHCP moves on */
        if (ctl_value(s->dhcp)) {
            s->net_shown = 0;
            net_fields(s);
        }
        s->net_shown = (int)dhcp_state() + 1;
        net_status(s);
    }
    if (ctl_value(s->tabs) != P_CLOCK)
        return;
    time_full_t t = time_rtc_local();
    uint32_t now = (uint32_t)t.hours * 3600 + t.minutes * 60 + t.seconds;
    if (now != s->clock_shown)
        clock_fill(s, !s->clock_edited);
}

static void build(settings_state_t *s) {
    boot_settings(s->boot_sig); /* as they are now */
    window *w = s->win;
    s->tabs =
        ctl_tabs(w, 4, 3, 264,
                 "Display|Sound|Clock|Network|Printers|Devices|System", NULL);

    /* Display: screen mode (kept on the boot disk), wallpaper */
    on_page(s, ctl_label(w, 10, 24, 60, 12, "Screen:", 0), P_WALL);
    s->screen = on_page(s, ctl_combo(w, 76, 24, 176, on_screen), P_WALL);
    /* only what this machine can take (config/video_cfg.h): 0 .. max */
    int max = video_max_mode();
    for (int m = 0; m <= max; m++)
        list_add(s->screen, video_mode_name(m));
    int vm = video_cfg_get();
    s->screen_saved = vm >= 0 ? vm : g_video_mode;
    if (s->screen_saved > max) /* shown as the most it can; the disk's choice
                                * stays unless changed here */
        s->screen_saved = max;
    list_set_sel(s->screen, s->screen_saved);
    ctl_enable(s->screen, vm >= 0);
    static char tip[96];
    if (vm < 0)
        snprintf(tip, sizeof(tip),
                 "This boot disk can't store it: "
                 "rebuild the image");
    else if (video_limit_reason())
        snprintf(tip, sizeof(tip), "Takes effect when you restart. No more: %s",
                 video_limit_reason());
    else
        snprintf(tip, sizeof(tip), "Takes effect when you restart");
    ctl_set_tooltip(s->screen, tip);
    control *sb = on_page(
        s, ctl_button(w, 178, 40, 74, 13, "Screen Saver...", 0, on_open_sub),
        P_WALL);
    sb->id = 0;
    control *pb = on_page(
        s, ctl_button(w, 178, 57, 74, 13, "Speed...", 0, on_open_sub), P_WALL);
    pb->id = 1;
    ctl_set_tooltip(pb, "What a slow machine can do without");
    on_page(s, ctl_label(w, 10, 40, 60, 12, "Pattern:", 0), P_WALL);
    s->pattern = on_page(s, ctl_combo(w, 76, 40, 96, on_wall), P_WALL);
    for (int i = 0; i < NPATTERNS; i++)
        list_add(s->pattern, PATTERNS[i]);
    control *eb = on_page(
        s, ctl_button(w, 178, 74, 74, 13, "Edit Pattern...", 0, on_open_sub),
        P_WALL);
    eb->id = 2;
    ctl_set_tooltip(eb, "Draw your own 8x8 desktop pattern");
    on_page(s, ctl_label(w, 10, 56, 60, 12, "Color A:", 0), P_WALL);
    s->col_a = on_page(s, ctl_combo(w, 76, 56, 96, on_wall), P_WALL);
    on_page(s, ctl_label(w, 10, 72, 60, 12, "Color B:", 0), P_WALL);
    s->col_b = on_page(s, ctl_combo(w, 76, 72, 96, on_wall), P_WALL);
    for (int i = 0; i < 16; i++) {
        list_add(s->col_a, COLORS[i]);
        list_add(s->col_b, COLORS[i]);
    }
    list_set_sel(s->pattern, g_cfg.wallpaper_pattern % NPATTERNS);
    list_set_sel(s->col_a, g_cfg.wallpaper_main_color & 15);
    list_set_sel(s->col_b, g_cfg.wallpaper_secondary_color & 15);
    on_page(s, ctl_label(w, 10, 88, 60, 8, "Preview:", 0), P_WALL);
    on_page(s, ctl_custom(w, 10, 97, 238, 22, paint_preview, NULL, CS_BORDER),
            P_WALL);
    on_page(s, ctl_label(w, 10, 125, 64, 12, "Characters:", 0), P_WALL);
    control *cp = on_page(s, ctl_combo(w, 76, 124, 130, on_codepage), P_WALL);
    for (int i = 0; i < CP_COUNT; i++)
        list_add(cp, cp_name[i]);
    list_set_sel(cp, g_codepage);
    ctl_set_tooltip(cp, "What codes 128-255 are: accents, symbols, box "
                        "drawing (Character Map shows them)");

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
    control *zm = flag(s, P_SYSTEM, 10, 120, "Large display: 2x (restart)",
                       &g_cfg.ui_zoom2);
    ctl_enable(zm, g_video_mode != 0); /* 640x400 and up */
    ctl_set_tooltip(zm, g_video_mode != 0
                            ? "Everything twice the size: 800x600 looks "
                              "like 400x300"
                            : "Only at 640x400 and up");
    ctl_set_tooltip(np, "Paging keeps apps out of each other's memory");

    /* Network: DHCP, or the addresses typed in */
    flag(s, P_NET, 10, 24, "Networking enabled (restart)",
         &g_cfg.networking_enabled);
    s->dhcp =
        on_page(s,
                ctl_check(w, 10, 37, 200, "Get an address automatically (DHCP)",
                          g_cfg.net_dhcp != 0, on_dhcp),
                P_NET);
    ctl_set_tooltip(s->dhcp, "Off: type the addresses in");
    control *rn =
        on_page(s, ctl_button(w, 210, 50, 56, 13, "Renew", 0, on_renew), P_NET);
    ctl_set_tooltip(rn, "Ask the network for an address again");
    static const char *lbl[4] = {
        "IP address:", "Gateway:", "Netmask:", "DNS server:"};
    for (int i = 0; i < 4; i++) {
        on_page(s, ctl_label(w, 10, 53 + i * 15, 60, 12, lbl[i], 0), P_NET);
        s->ip[i] = on_page(
            s,
            ctl_edit(w, 76, 52 + i * 15, 90, 12, "", 15, ES_DECIMAL, on_edit),
            P_NET);
    }
    ctl_set_tooltip(s->ip[3], "QEMU and 86Box: 10.0.2.3, or 8.8.8.8");
    s->net_state = on_page(s, ctl_label(w, 10, 115, 256, 9, "", 0), P_NET);
    net_fields(s);
    s->net_shown = 1;

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

    /* Printers */
    on_page(s, ctl_label(w, 10, 24, 60, 12, "Printer:", 0), P_PRINT);
    s->p_type = on_page(s, ctl_combo(w, 76, 24, 172, on_prn_type), P_PRINT);
    for (int i = 0; i < PRN_TYPES; i++)
        list_add(s->p_type, PRN_TYPE_NAMES[i]);
    list_set_sel(s->p_type, g_cfg.printer_type % PRN_TYPES);
    ctl_set_tooltip(s->p_type, "Text works on any printer; the other two "
                               "also print pictures");
    on_page(s, ctl_label(w, 10, 40, 60, 12, "Port:", 0), P_PRINT);
    s->p_port = on_page(s, ctl_combo(w, 76, 40, 100, on_prn_port), P_PRINT);
    for (int i = 0; i < LPT_MAX; i++) {
        char t[20];
        if (lpt_base(i))
            snprintf(t, sizeof(t), "LPT%d (%Xh)", i + 1, lpt_base(i));
        else
            snprintf(t, sizeof(t), "LPT%d (none)", i + 1);
        list_add(s->p_port, t);
    }
    list_set_sel(s->p_port,
                 g_cfg.printer_port < LPT_MAX ? g_cfg.printer_port : 0);
    control *ff =
        on_page(s,
                ctl_check(w, 10, 58, 230, "Form feed after each job (new page)",
                          !g_cfg.printer_no_ff, on_prn_ff),
                P_PRINT);
    ctl_set_tooltip(ff, "Off: the next job goes on the same page");
    s->p_status = on_page(s, ctl_label(w, 10, 76, 238, 9, "", 0), P_PRINT);
    on_page(s, ctl_button(w, 10, 92, 80, 13, "Print Queue...", 0, on_prn_queue),
            P_PRINT);
    on_page(s, ctl_button(w, 96, 92, 70, 13, "Test Page", 0, on_prn_test),
            P_PRINT);
    on_page(s,
            ctl_label(w, 10, 112, 240, 18,
                      "Pick the printer's language: ESC/P for Epson-style\n"
                      "dot matrix, PCL for HP lasers. Unsure? Text only.",
                      0),
            P_PRINT);
    prn_status(s);

    /* Devices */
    s->dev_list =
        on_page(s, ctl_list(w, 6, 24, 260, 82, LBS_HEADER, on_dev), P_DEVICES);
    int dcols[3] = {80, 82, 0};
    list_set_columns(s->dev_list, dcols, 3);
    dev_fill(s);
    on_page(s,
            ctl_label(w, 8, 112, 170, 18,
                      "Double-click a device to set it up.\n"
                      "Refresh looks for device changes.",
                      0),
            P_DEVICES);
    on_page(s, ctl_button(w, 206, 114, 60, 13, "Refresh", 0, on_dev_refresh),
            P_DEVICES);

    /* bottom bar */
    s->status = ctl_label(w, 6, 140, 180, 13, "", 0);
    s->save = ctl_button(w, 219, 138, 50, 13, "Save", 1, on_save);
    ctl_style_add(s->save, CS_DEFAULT);
    set_dirty(s, false);
}

static void on_close(void) { os_quit_app_by_desc(&settings_app); }

static void settings_destroy(void *state) {
    settings_state_t *s = state;
    close_sub(s);
}

static void settings_init(void *state) {
    settings_state_t *s = state;
    if (g_cfg.sb16_volume == 0)
        g_cfg.sb16_volume = 8;
    s->win = app_window(s, "Settings", 30, 20, 275, 172, DARK_GRAY, 0);
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
    .destroy = settings_destroy,
    .single_instance = true,
};
