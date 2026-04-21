#include "config/runtime_config.h"
#include "os/api.h"

#define SET_W 220
#define SET_H 180
#define SET_X 30
#define SET_Y 20

#define ROW_H 12
#define LBL_X 6
#define VAL_X 120
#define ROW0_Y 4

typedef struct {
    window *win;
    bool dirty;
    char status[48];
    uint32_t status_timer;
} settings_state_t;

app_descriptor settings_app;

static void set_status(settings_state_t *s, const char *msg) {
    strncpy(s->status, msg, 47);
    s->status[47] = '\0';
    s->status_timer = 180;
}

static bool btn(int x, int y, int w, int h, const char *lbl, uint8_t bg) {
    fill_rect(x, y, w, h, bg);
    draw_rect(x, y, w, h, BLACK);
    int tx = x + w / 2 - (int)(strlen(lbl) * 5) / 2, ty = y + h / 2 - 3;
    draw_string(tx, ty, (char *)lbl, WHITE, 2);
    return mouse.left_clicked && mouse.x >= x && mouse.x < x + w &&
           mouse.y >= y && mouse.y < y + h;
}

static void settings_draw(window *win, void *ud) {
    settings_state_t *s = (settings_state_t *)ud;
    if (!s)
        return;
    int wx = win->x + 1, wy = win->y + MENUBAR_H + 16, ww = win->w - 2,
        wh = win->h - 16;
    fill_rect(wx, wy, ww, wh, LIGHT_GRAY);
    fill_rect(wx, wy, ww, 10, DARK_GRAY);
    draw_string(wx + 4, wy + 2, "System Settings", WHITE, 2);
    int row = 1;
#define ROW_Y(r) (wy + 4 + (r) * ROW_H + 8)

    draw_string(wx + LBL_X, ROW_Y(row), "Wallpaper:", DARK_GRAY, 2);
    row++;
    static const char *WNAMES[] = {"Solid", "Checker", "Stripes", "Dots"};
    int wp = g_cfg.wallpaper_pattern & 3;
    draw_string(wx + LBL_X + 8, ROW_Y(row), (char *)WNAMES[wp], BLACK, 2);
    btn(wx + VAL_X, ROW_Y(row) - 1, 10, 9, "<", DARK_GRAY);
    btn(wx + VAL_X + 12, ROW_Y(row) - 1, 10, 9, ">", DARK_GRAY);
    row++;

    draw_string(wx + LBL_X, ROW_Y(row), "Wall Color A:", DARK_GRAY, 2);
    fill_rect(wx + VAL_X, ROW_Y(row) - 1, 16, 9, g_cfg.wallpaper_main_color);
    draw_rect(wx + VAL_X, ROW_Y(row) - 1, 16, 9, BLACK);
    btn(wx + VAL_X + 18, ROW_Y(row) - 1, 10, 9, "-", DARK_GRAY);
    btn(wx + VAL_X + 30, ROW_Y(row) - 1, 10, 9, "+", DARK_GRAY);
    row++;

    draw_string(wx + LBL_X, ROW_Y(row), "Wall Color B:", DARK_GRAY, 2);
    fill_rect(wx + VAL_X, ROW_Y(row) - 1, 16, 9,
              g_cfg.wallpaper_secondary_color);
    draw_rect(wx + VAL_X, ROW_Y(row) - 1, 16, 9, BLACK);
    btn(wx + VAL_X + 18, ROW_Y(row) - 1, 10, 9, "-", DARK_GRAY);
    btn(wx + VAL_X + 30, ROW_Y(row) - 1, 10, 9, "+", DARK_GRAY);
    row++;

    draw_string(wx + LBL_X, ROW_Y(row), "Timezone:", DARK_GRAY, 2);
    char tz_buf[8];
    int8_t tz = (int8_t)g_cfg.timezone_offset;
    sprintf(tz_buf, "%+d", (int)tz);
    draw_string(wx + VAL_X, ROW_Y(row), tz_buf, BLACK, 2);
    btn(wx + VAL_X + 20, ROW_Y(row) - 1, 10, 9, "-", DARK_GRAY);
    btn(wx + VAL_X + 32, ROW_Y(row) - 1, 10, 9, "+", DARK_GRAY);
    row++;

    draw_string(wx + LBL_X, ROW_Y(row), "Boot Chime:", DARK_GRAY, 2);
    draw_string(wx + VAL_X, ROW_Y(row), g_cfg.play_bootchime ? "ON" : "OFF",
                BLACK, 2);
    btn(wx + VAL_X + 20, ROW_Y(row) - 1, 24, 9,
        g_cfg.play_bootchime ? "Off" : "On", DARK_GRAY);
    row++;

    draw_string(wx + LBL_X, ROW_Y(row), "Sound:", DARK_GRAY, 2);
    draw_string(wx + VAL_X, ROW_Y(row), g_cfg.sound_enabled ? "ON" : "OFF",
                BLACK, 2);
    btn(wx + VAL_X + 20, ROW_Y(row) - 1, 24, 9,
        g_cfg.sound_enabled ? "Off" : "On", DARK_GRAY);
    row++;

    draw_string(wx + LBL_X, ROW_Y(row), "Start in GUI:", DARK_GRAY, 2);
    draw_string(wx + VAL_X, ROW_Y(row), g_cfg.start_in_gui ? "YES" : "NO",
                BLACK, 2);
    btn(wx + VAL_X + 22, ROW_Y(row) - 1, 24, 9,
        g_cfg.start_in_gui ? "No" : "Yes", DARK_GRAY);
    row++;

    draw_string(wx + LBL_X, ROW_Y(row), "FileF Mode:", DARK_GRAY, 2);
    draw_string(wx + VAL_X, ROW_Y(row),
                g_cfg.filef_single_window ? "Single" : "Multi", BLACK, 2);
    btn(wx + VAL_X + 30, ROW_Y(row) - 1, 36, 9,
        g_cfg.filef_single_window ? "Multi" : "Single", DARK_GRAY);
    row += 2;

    uint8_t save_bg = s->dirty ? RED : DARK_GRAY;
    btn(wx + ww / 2 - 30, ROW_Y(row) - 1, 60, 10, "Save", save_bg);

    int sy = wy + wh - 10;
    fill_rect(wx, sy, ww, 10, DARK_GRAY);
    if (s->status_timer > 0)
        draw_string(wx + 4, sy + 2, s->status, LIGHT_GREEN, 2);
    else if (s->dirty)
        draw_string(wx + 4, sy + 2, "Unsaved changes", LIGHT_YELLOW, 2);
    else
        draw_string(wx + 4, sy + 2, "All saved", LIGHT_GRAY, 2);

#undef ROW_Y
}

static void settings_on_frame(void *state) {
    settings_state_t *s = (settings_state_t *)state;
    if (!s->win || !s->win->visible)
        return;
    if (s->status_timer > 0)
        s->status_timer--;

    int wx = s->win->x + 1, wy = s->win->y + MENUBAR_H + 16, ww = s->win->w - 2,
        wh = s->win->h - 16;

#define ROW_Y(r) (wy + 4 + (r) * ROW_H + 8)
    if (mouse.left_clicked) {
        int row = 1;
        row++;
        if (btn(wx + VAL_X, ROW_Y(row) - 1, 10, 9, "<", DARK_GRAY)) {
            g_cfg.wallpaper_pattern = (g_cfg.wallpaper_pattern + 3) % 4;
            s->dirty = true;
        }
        if (btn(wx + VAL_X + 12, ROW_Y(row) - 1, 10, 9, ">", DARK_GRAY)) {
            g_cfg.wallpaper_pattern = (g_cfg.wallpaper_pattern + 1) % 4;
            s->dirty = true;
        }
        row++;
        if (btn(wx + VAL_X + 18, ROW_Y(row) - 1, 10, 9, "-", DARK_GRAY)) {
            if (g_cfg.wallpaper_main_color > 0) {
                g_cfg.wallpaper_main_color--;
                s->dirty = true;
            }
        }
        if (btn(wx + VAL_X + 30, ROW_Y(row) - 1, 10, 9, "+", DARK_GRAY)) {
            if (g_cfg.wallpaper_main_color < 15) {
                g_cfg.wallpaper_main_color++;
                s->dirty = true;
            }
        }
        row++;
        if (btn(wx + VAL_X + 18, ROW_Y(row) - 1, 10, 9, "-", DARK_GRAY)) {
            if (g_cfg.wallpaper_secondary_color > 0) {
                g_cfg.wallpaper_secondary_color--;
                s->dirty = true;
            }
        }
        if (btn(wx + VAL_X + 30, ROW_Y(row) - 1, 10, 9, "+", DARK_GRAY)) {
            if (g_cfg.wallpaper_secondary_color < 15) {
                g_cfg.wallpaper_secondary_color++;
                s->dirty = true;
            }
        }
        row++;
        if (btn(wx + VAL_X + 20, ROW_Y(row) - 1, 10, 9, "-", DARK_GRAY)) {
            int8_t tz = (int8_t)g_cfg.timezone_offset;
            if (tz > -12) {
                g_cfg.timezone_offset = (uint8_t)(tz - 1);
                s->dirty = true;
            }
        }
        if (btn(wx + VAL_X + 32, ROW_Y(row) - 1, 10, 9, "+", DARK_GRAY)) {
            int8_t tz = (int8_t)g_cfg.timezone_offset;
            if (tz < 14) {
                g_cfg.timezone_offset = (uint8_t)(tz + 1);
                s->dirty = true;
            }
        }
        row++;
        if (btn(wx + VAL_X + 20, ROW_Y(row) - 1, 24, 9,
                g_cfg.play_bootchime ? "Off" : "On", DARK_GRAY)) {
            g_cfg.play_bootchime ^= 1;
            s->dirty = true;
        }
        row++;
        if (btn(wx + VAL_X + 20, ROW_Y(row) - 1, 24, 9,
                g_cfg.sound_enabled ? "Off" : "On", DARK_GRAY)) {
            g_cfg.sound_enabled ^= 1;
            s->dirty = true;
        }
        row++;
        if (btn(wx + VAL_X + 22, ROW_Y(row) - 1, 24, 9,
                g_cfg.start_in_gui ? "No" : "Yes", DARK_GRAY)) {
            g_cfg.start_in_gui ^= 1;
            s->dirty = true;
        }
        row++;
        if (btn(wx + VAL_X + 30, ROW_Y(row) - 1, 36, 9,
                g_cfg.filef_single_window ? "Multi" : "Single", DARK_GRAY)) {
            g_cfg.filef_single_window ^= 1;
            s->dirty = true;
        }
        row += 2;
        if (btn(wx + ww / 2 - 30, ROW_Y(row) - 1, 60, 10, "Save",
                s->dirty ? RED : DARK_GRAY)) {
            if (cfg_save()) {
                s->dirty = false;
                set_status(s, "Saved successfully.");
            } else
                set_status(s, "Save failed!");
        }
    }
#undef ROW_Y
    settings_draw(s->win, s);
}

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
    s->dirty = false;
}

static void settings_destroy(void *state) {
    settings_state_t *s = (settings_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor settings_app = {
    .name = "SETTINGS",
    .state_size = sizeof(settings_state_t),
    .init = settings_init,
    .on_frame = settings_on_frame,
    .destroy = settings_destroy,
};
