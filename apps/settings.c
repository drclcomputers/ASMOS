#include "config/runtime_config.h"
#include "drivers/sb16.h"
#include "os/api.h"

#define SET_W 260
#define SET_H 200
#define SET_X 30
#define SET_Y 20

// ── Layout ────────────────────────────────────────────────────────────────
#define TAB_X 1
#define TAB_Y 1
#define TAB_W 54
#define TAB_H 9
#define TAB_GAP 1

#define PANEL_X (TAB_W + 4)
#define PANEL_Y 1
#define PANEL_W (SET_W - PANEL_X - 4)
#define PANEL_H (SET_H - 30)

#define ROW_H 12
#define LBL_X 4
#define VAL_X 120
#define FIRST_ROW_Y 6

#define STATUS_H 10

typedef enum {
    TAB_WALLPAPER = 0,
    TAB_SOUND,
    TAB_SYSTEM,
    TAB_COUNT
} settings_tab_t;

static const char *TAB_NAMES[TAB_COUNT] = {
    "Wallpaper",
    "Sound",
    "System",
};

typedef struct {
    window *win;
    settings_tab_t tab;
    bool dirty;
    char status[64];
    uint32_t status_timer;
} settings_state_t;

app_descriptor settings_app;

// ── Helpers ───────────────────────────────────────────────────────────────
static void set_status(settings_state_t *s, const char *msg) {
    strncpy(s->status, msg, 63);
    s->status[63] = '\0';
    s->status_timer = 180;
}

static bool small_btn(int x, int y, int w, int h, const char *lbl, uint8_t bg) {
    fill_rect(x, y, w, h, bg);
    draw_rect(x, y, w, h, BLACK);
    int tx = x + w / 2 - (int)(strlen(lbl) * 5) / 2;
    int ty = y + h / 2 - 3;
    draw_string(tx, ty, (char *)lbl, WHITE, 2);
    return mouse.left_clicked && mouse.x >= x && mouse.x < x + w &&
           mouse.y >= y && mouse.y < y + h;
}

static void draw_wallpaper_tab(int px, int py) {
    static const char *WNAMES[] = {"Solid", "Checker", "Stripes", "V.Stripes",
                                   "Dots"};
    int row = 0;
#define RY(r) (py + FIRST_ROW_Y + (r) * ROW_H)

    draw_string(px + LBL_X, RY(row), "Pattern:", DARK_GRAY, 2);
    int wp = g_cfg.wallpaper_pattern & 3;
    draw_string(px + VAL_X - 55, RY(row), (char *)WNAMES[wp], BLACK, 2);
    small_btn(px + VAL_X, RY(row) - 1, 10, 9, "<", DARK_GRAY);
    small_btn(px + VAL_X + 12, RY(row) - 1, 10, 9, ">", DARK_GRAY);
    row++;

    draw_string(px + LBL_X, RY(row), "Color A:", DARK_GRAY, 2);
    fill_rect(px + VAL_X - 20, RY(row) - 1, 16, 9, g_cfg.wallpaper_main_color);
    draw_rect(px + VAL_X - 20, RY(row) - 1, 16, 9, BLACK);
    small_btn(px + VAL_X, RY(row) - 1, 10, 9, "-", DARK_GRAY);
    small_btn(px + VAL_X + 12, RY(row) - 1, 10, 9, "+", DARK_GRAY);
    row++;

    draw_string(px + LBL_X, RY(row), "Color B:", DARK_GRAY, 2);
    fill_rect(px + VAL_X - 20, RY(row) - 1, 16, 9,
              g_cfg.wallpaper_secondary_color);
    draw_rect(px + VAL_X - 20, RY(row) - 1, 16, 9, BLACK);
    small_btn(px + VAL_X, RY(row) - 1, 10, 9, "-", DARK_GRAY);
    small_btn(px + VAL_X + 12, RY(row) - 1, 10, 9, "+", DARK_GRAY);
    row++;

    row++;
    draw_string(px + LBL_X, RY(row), "Preview:", DARK_GRAY, 2);
    row++;
    int sw_x = px + LBL_X, sw_y = RY(row) - 2, sw_w = PANEL_W - 12, sw_h = 18;
    switch (g_cfg.wallpaper_pattern) {
    case WALLPAPER_CHECKERBOARD: // checkerboard
        for (int yy = 0; yy < sw_h; yy += 4)
            for (int xx = 0; xx < sw_w; xx += 4) {
                uint8_t c = ((xx / 4) + (yy / 4)) % 2 == 0
                                ? g_cfg.wallpaper_main_color
                                : g_cfg.wallpaper_secondary_color;
                fill_rect(sw_x + xx, sw_y + yy, 4, 4, c);
            }
        break;
    case WALLPAPER_STRIPES: // stripes
        for (int yy = 0; yy < sw_h; yy++)
            draw_line(sw_x, sw_y + yy, sw_x + sw_w - 1, sw_y + yy,
                      (yy / 2) % 2 == 0 ? g_cfg.wallpaper_main_color
                                        : g_cfg.wallpaper_secondary_color);
        break;
    case WALLPAPER_VERTICAL_STRIPES: // vertical stripes
        for (int xx = 0; xx < sw_w; xx++)
            draw_line(sw_x + xx, sw_y, sw_x + xx, sw_y + sw_h - 1,
                      (xx / 3) % 2 == 0 ? g_cfg.wallpaper_main_color
                                        : g_cfg.wallpaper_secondary_color);
        break;
    case WALLPAPER_DOTS: // dots
        fill_rect(sw_x, sw_y, sw_w, sw_h, g_cfg.wallpaper_main_color);
        for (int yy = 2; yy < sw_h; yy += 4)
            for (int xx = 2; xx < sw_w; xx += 4)
                draw_dot(sw_x + xx, sw_y + yy, g_cfg.wallpaper_secondary_color);
        break;
    default:
        fill_rect(sw_x, sw_y, sw_w, sw_h, g_cfg.wallpaper_main_color);
        break;
    }
    draw_rect(sw_x, sw_y, sw_w, sw_h, BLACK);
#undef RY
}

static void draw_sound_tab(int px, int py) {
    int row = 0;
#define RY(r) (py + FIRST_ROW_Y + (r) * ROW_H)

    // ── PC Speaker ────────────────────────────────────────────────────────
    draw_string(px + LBL_X, RY(row), "-- PC Speaker --", LIGHT_CYAN, 2);
    row++;

    draw_string(px + LBL_X, RY(row), "Sound:", DARK_GRAY, 2);
    draw_string(px + VAL_X - 30, RY(row), g_cfg.sound_enabled ? "ON" : "OFF",
                BLACK, 2);
    small_btn(px + VAL_X, RY(row) - 1, 24, 9,
              g_cfg.sound_enabled ? "Off" : "On", DARK_GRAY);
    row++;

    draw_string(px + LBL_X, RY(row), "Boot Chime:", DARK_GRAY, 2);
    draw_string(px + VAL_X - 30, RY(row), g_cfg.play_bootchime ? "ON" : "OFF",
                BLACK, 2);
    small_btn(px + VAL_X, RY(row) - 1, 24, 9,
              g_cfg.play_bootchime ? "Off" : "On", DARK_GRAY);
    row++;

    row++;
    // ── Sound Blaster 16 ──────────────────────────────────────────────────
    draw_string(px + LBL_X, RY(row), "-- Sound Blaster 16 --", LIGHT_CYAN, 2);
    row++;

    bool det = sb16_detected();
    if (!det) {
        draw_string(px + LBL_X, RY(row), "Status: NOT INSTALLED", RED, 2);
        row++;
        draw_string(px + LBL_X, RY(row), "No SB16-compatible card", DARK_GRAY,
                    2);
        row++;
        draw_string(px + LBL_X, RY(row), "found at 0x220-0x280.", DARK_GRAY, 2);
        row++;
        draw_string(px + LBL_X, RY(row), "Check QEMU -soundhw sb16", DARK_GRAY,
                    2);
        row++;
        draw_string(px + LBL_X, RY(row), "or -device sb16.", DARK_GRAY, 2);
    } else {
        // Green status dot + label
        fill_rect(px + LBL_X, RY(row) + 1, 6, 6, LIGHT_GREEN);
        draw_string(px + LBL_X + 9, RY(row), "Status: DETECTED", LIGHT_GREEN,
                    2);
        row++;

        draw_string(px + LBL_X, RY(row), "Base port:", DARK_GRAY, 2);
        draw_string(px + VAL_X - 10, RY(row), "0x220", BLACK, 2);
        row++;

        draw_string(px + LBL_X, RY(row), "IRQ:", DARK_GRAY, 2);
        draw_string(px + VAL_X - 10, RY(row), "5", BLACK, 2);
        row++;

        draw_string(px + LBL_X, RY(row), "DMA:", DARK_GRAY, 2);
        draw_string(px + VAL_X - 10, RY(row), "1", BLACK, 2);
        row++;

        draw_string(px + LBL_X, RY(row), "Capabilities:", DARK_GRAY, 2);
        draw_string(px + VAL_X - 10, RY(row), "8/16-bit PCM", BLACK, 2);
        row++;

        draw_string(px + LBL_X, RY(row), "Volume:", DARK_GRAY, 2);
        // 4-bit volume stored in g_cfg, range 0-15 mapped to 0x00-0xF0
        char vol_str[8];
        uint8_t vol = g_cfg.sb16_volume;
        sprintf(vol_str, "%d", (int)vol);
        draw_string(px + VAL_X - 10, RY(row), vol_str, BLACK, 2);
        small_btn(px + VAL_X + 10, RY(row) - 1, 10, 9, "-", DARK_GRAY);
        small_btn(px + VAL_X + 22, RY(row) - 1, 10, 9, "+", DARK_GRAY);

        // Volume bar
        row++;
        int vb_x = px + LBL_X, vb_y = RY(row) - 2, vb_w = PANEL_W - 12,
            vb_h = 6;
        fill_rect(vb_x, vb_y, vb_w, vb_h, DARK_GRAY);
        int filled = (vol * vb_w) / 15;
        if (filled > 0)
            fill_rect(vb_x, vb_y, filled, vb_h, LIGHT_GREEN);
        draw_rect(vb_x, vb_y, vb_w, vb_h, BLACK);
    }
#undef RY
}

static void draw_system_tab(int px, int py) {
    int row = 0;
#define RY(r) (py + FIRST_ROW_Y + (r) * ROW_H)

    draw_string(px + LBL_X, RY(row), "-- Display --", LIGHT_CYAN, 2);
    row++;

    draw_string(px + LBL_X, RY(row), "Reduce Motion:", DARK_GRAY, 2);
    draw_string(px + VAL_X - 30, RY(row), g_cfg.reduce_motion ? "ON" : "OFF",
                BLACK, 2);
    small_btn(px + VAL_X, RY(row) - 1, 24, 9,
              g_cfg.reduce_motion ? "Off" : "On", DARK_GRAY);
    row++;

    row++;
    draw_string(px + LBL_X, RY(row), "-- Boot --", LIGHT_CYAN, 2);
    row++;

    draw_string(px + LBL_X, RY(row), "Start in GUI:", DARK_GRAY, 2);
    draw_string(px + VAL_X - 30, RY(row), g_cfg.start_in_gui ? "YES" : "NO",
                BLACK, 2);
    small_btn(px + VAL_X, RY(row) - 1, 24, 9, g_cfg.start_in_gui ? "No" : "Yes",
              DARK_GRAY);
    row++;

    draw_string(px + LBL_X, RY(row), "Timezone:", DARK_GRAY, 2);
    char tz_buf[8];
    int8_t tz = (int8_t)g_cfg.timezone_offset;
    sprintf(tz_buf, "%+d", (int)tz);
    draw_string(px + VAL_X - 10, RY(row), tz_buf, BLACK, 2);
    small_btn(px + VAL_X + 16, RY(row) - 1, 10, 9, "-", DARK_GRAY);
    small_btn(px + VAL_X + 28, RY(row) - 1, 10, 9, "+", DARK_GRAY);
    row++;

    row++;
    draw_string(px + LBL_X, RY(row), "-- File Manager --", LIGHT_CYAN, 2);
    row++;

    draw_string(px + LBL_X, RY(row), "FileF Mode:", DARK_GRAY, 2);
    draw_string(px + VAL_X - 30, RY(row),
                g_cfg.filef_single_window ? "Single" : "Multi", BLACK, 2);
    small_btn(px + VAL_X, RY(row) - 1, 36, 9,
              g_cfg.filef_single_window ? "Multi" : "Single", DARK_GRAY);
#undef RY
}

// ── Main draw ─────────────────────────────────────────────────────────────

static void settings_draw(window *win, void *ud) {
    settings_state_t *s = (settings_state_t *)ud;
    if (!s)
        return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(wx, wy, ww, wh, LIGHT_GRAY);

    // ── Tab sidebar ───────────────────────────────────────────────────────
    int tab_base_x = wx + TAB_X;
    int tab_base_y = wy + TAB_Y;

    for (int i = 0; i < TAB_COUNT; i++) {
        int ty = tab_base_y + i * (TAB_H + TAB_GAP);
        bool active = (s->tab == (settings_tab_t)i);
        uint8_t bg = active ? CYAN : DARK_GRAY;
        fill_rect(tab_base_x, ty, TAB_W, TAB_H, bg);
        draw_rect(tab_base_x, ty, TAB_W, TAB_H, BLACK);
        draw_string(tab_base_x + 3, ty + 2, (char *)TAB_NAMES[i],
                    active ? BLACK : LIGHT_GRAY, 2);
    }

    // Divider
    draw_line(wx + PANEL_X - 2, wy, wx + PANEL_X - 2, wy + wh - STATUS_H - 2,
              DARK_GRAY);

    // ── Panel area ────────────────────────────────────────────────────────
    int px = wx + PANEL_X;
    int py = wy + PANEL_Y;
    fill_rect(px, py, PANEL_W, PANEL_H, LIGHT_GRAY);

    switch (s->tab) {
    case TAB_WALLPAPER:
        draw_wallpaper_tab(px, py);
        break;
    case TAB_SOUND:
        draw_sound_tab(px, py);
        break;
    case TAB_SYSTEM:
        draw_system_tab(px, py);
        break;
    default:
        break;
    }

    // ── Bottom bar ────────────────────────────────────────────────────────
    int bar_y = wy + wh - STATUS_H - 1;
    fill_rect(wx, bar_y, ww, STATUS_H + 1, DARK_GRAY);
    draw_line(wx, bar_y, wx + ww - 1, bar_y, BLACK);

    // Save button
    uint8_t save_bg = s->dirty ? RED : DARK_GRAY;
    small_btn(wx + ww - 46, bar_y + 1, 44, 8, "Save", save_bg);

    // Status text
    if (s->status_timer > 0)
        draw_string(wx + 4, bar_y + 2, s->status, LIGHT_GREEN, 2);
    else if (s->dirty)
        draw_string(wx + 4, bar_y + 2, "Unsaved changes", YELLOW, 2);
    else
        draw_string(wx + 4, bar_y + 2, "All saved", LIGHT_GRAY, 2);
}

// ── Input handlers per tab ────────────────────────────────────────────────

static void handle_wallpaper_input(settings_state_t *s, int px, int py) {
    int row = 0;
#define RY(r) (py + FIRST_ROW_Y + (r) * ROW_H)
    if (small_btn(px + VAL_X, RY(row) - 1, 10, 9, "<", DARK_GRAY)) {
        g_cfg.wallpaper_pattern = (g_cfg.wallpaper_pattern + 4) % 5;
        s->dirty = true;
    }
    if (small_btn(px + VAL_X + 12, RY(row) - 1, 10, 9, ">", DARK_GRAY)) {
        g_cfg.wallpaper_pattern = (g_cfg.wallpaper_pattern + 1) % 5;
        s->dirty = true;
    }
    row++;
    if (small_btn(px + VAL_X, RY(row) - 1, 10, 9, "-", DARK_GRAY)) {
        if (g_cfg.wallpaper_main_color > 0) {
            g_cfg.wallpaper_main_color--;
            s->dirty = true;
        }
    }
    if (small_btn(px + VAL_X + 12, RY(row) - 1, 10, 9, "+", DARK_GRAY)) {
        if (g_cfg.wallpaper_main_color < 15) {
            g_cfg.wallpaper_main_color++;
            s->dirty = true;
        }
    }
    row++;
    if (small_btn(px + VAL_X, RY(row) - 1, 10, 9, "-", DARK_GRAY)) {
        if (g_cfg.wallpaper_secondary_color > 0) {
            g_cfg.wallpaper_secondary_color--;
            s->dirty = true;
        }
    }
    if (small_btn(px + VAL_X + 12, RY(row) - 1, 10, 9, "+", DARK_GRAY)) {
        if (g_cfg.wallpaper_secondary_color < 15) {
            g_cfg.wallpaper_secondary_color++;
            s->dirty = true;
        }
    }
#undef RY
}

static void handle_sound_input(settings_state_t *s, int px, int py) {
    int row = 1; // skip header row
#define RY(r) (py + FIRST_ROW_Y + (r) * ROW_H)
    if (small_btn(px + VAL_X, RY(row) - 1, 24, 9,
                  g_cfg.sound_enabled ? "Off" : "On", DARK_GRAY)) {
        g_cfg.sound_enabled ^= 1;
        s->dirty = true;
    }
    row++;
    if (small_btn(px + VAL_X, RY(row) - 1, 24, 9,
                  g_cfg.play_bootchime ? "Off" : "On", DARK_GRAY)) {
        g_cfg.play_bootchime ^= 1;
        s->dirty = true;
    }

    if (!sb16_detected()) {
        return;
    }

    row += 3; // skip separator + SB16 header + status
    row += 3; // skip port / IRQ / DMA rows
    // Volume row
    if (small_btn(px + VAL_X + 10, RY(row) - 1, 10, 9, "-", DARK_GRAY)) {
        if (g_cfg.sb16_volume > 0) {
            g_cfg.sb16_volume--;
            uint8_t v = (g_cfg.sb16_volume << 4) | g_cfg.sb16_volume;
            sb16_set_volume(v, v);
            s->dirty = true;
        }
    }
    if (small_btn(px + VAL_X + 22, RY(row) - 1, 10, 9, "+", DARK_GRAY)) {
        if (g_cfg.sb16_volume < 15) {
            g_cfg.sb16_volume++;
            uint8_t v = (g_cfg.sb16_volume << 4) | g_cfg.sb16_volume;
            sb16_set_volume(v, v);
            s->dirty = true;
        }
    }
#undef RY
}

static void handle_system_input(settings_state_t *s, int px, int py) {
    int row = 1;
#define RY(r) (py + FIRST_ROW_Y + (r) * ROW_H)
    if (small_btn(px + VAL_X, RY(row) - 1, 24, 9,
                  g_cfg.reduce_motion ? "Off" : "On", DARK_GRAY)) {
        g_cfg.reduce_motion ^= 1;
        s->dirty = true;
    }
    row += 3; // skip spacer + boot header
    if (small_btn(px + VAL_X, RY(row) - 1, 24, 9,
                  g_cfg.start_in_gui ? "No" : "Yes", DARK_GRAY)) {
        g_cfg.start_in_gui ^= 1;
        s->dirty = true;
    }
    row++;
    if (small_btn(px + VAL_X + 16, RY(row) - 1, 10, 9, "-", DARK_GRAY)) {
        int8_t tz = (int8_t)g_cfg.timezone_offset;
        if (tz > -12) {
            g_cfg.timezone_offset = (uint8_t)(tz - 1);
            s->dirty = true;
        }
    }
    if (small_btn(px + VAL_X + 28, RY(row) - 1, 10, 9, "+", DARK_GRAY)) {
        int8_t tz = (int8_t)g_cfg.timezone_offset;
        if (tz < 14) {
            g_cfg.timezone_offset = (uint8_t)(tz + 1);
            s->dirty = true;
        }
    }
    row += 3; // skip spacer + filemanager header
    if (small_btn(px + VAL_X, RY(row) - 1, 36, 9,
                  g_cfg.filef_single_window ? "Multi" : "Single", DARK_GRAY)) {
        g_cfg.filef_single_window ^= 1;
        s->dirty = true;
    }
#undef RY
}

// ── on_frame ──────────────────────────────────────────────────────────────

static void settings_on_frame(void *state) {
    settings_state_t *s = (settings_state_t *)state;
    if (!s->win || !s->win->visible)
        return;
    if (s->status_timer > 0)
        s->status_timer--;

    bool focused = window_is_focused(s->win);

    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16;
    int ww = s->win->w - 2;
    int wh = s->win->h - 16;

    int px = wx + PANEL_X;
    int py = wy + PANEL_Y;

    int bar_y = wy + wh - STATUS_H - 1;

    if (mouse.left_clicked && focused) {
        // Tab clicks
        int tab_base_x = wx + TAB_X;
        int tab_base_y = wy + TAB_Y;
        for (int i = 0; i < TAB_COUNT; i++) {
            int ty = tab_base_y + i * (TAB_H + TAB_GAP);
            if (mouse.x >= tab_base_x && mouse.x < tab_base_x + TAB_W &&
                mouse.y >= ty && mouse.y < ty + TAB_H) {
                s->tab = (settings_tab_t)i;
            }
        }

        // Save button
        if (small_btn(wx + ww - 46, bar_y + 1, 44, 8, "Save",
                      s->dirty ? RED : DARK_GRAY)) {
            if (cfg_save()) {
                s->dirty = false;
                set_status(s, "Saved successfully.");
            } else {
                set_status(s, "Save failed!");
            }
        }

        // Panel input
        switch (s->tab) {
        case TAB_WALLPAPER:
            handle_wallpaper_input(s, px, py);
            break;
        case TAB_SOUND:
            handle_sound_input(s, px, py);
            break;
        case TAB_SYSTEM:
            handle_system_input(s, px, py);
            break;
        default:
            break;
        }
    }

    settings_draw(s->win, s);
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

static bool settings_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&settings_app);
    return true;
}
static void on_file_close(void) { settings_close(NULL); }

static void settings_init(void *state) {
    settings_state_t *s = (settings_state_t *)state;
    const window_spec_t spec = {
        .x = SET_X,
        .y = SET_Y,
        .w = SET_W,
        .h = SET_H,
        .resizable = false,
        .title = "Settings",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = LIGHT_GRAY,
        .visible = true,
        .on_close = settings_close,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;
    s->win->on_draw = settings_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);

    s->tab = TAB_WALLPAPER;
    s->dirty = false;

    if (g_cfg.sb16_volume == 0)
        g_cfg.sb16_volume = 8;
}

static void settings_destroy(void *state) {
    settings_state_t *s = (settings_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor settings_app = {
    .name = "Settings",
    .state_size = sizeof(settings_state_t),
    .init = settings_init,
    .on_frame = settings_on_frame,
    .destroy = settings_destroy,
    .single_instance = true,
};
