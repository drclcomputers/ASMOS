#include "ui/desktop.h"
#include "ui/desktop_fs.h"
#include "ui/window.h"
#include "ui/menubar.h"
#include "os/os.h"
#include "config/config.h"
#include "lib/primitive_graphics.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "io/mouse.h"
#include "interrupts/idt.h"
#include "ui/modal.h"

#define WALLPAPER_SOLID         0
#define WALLPAPER_CHECKERBOARD  1
#define WALLPAPER_STRIPES       2
#define WALLPAPER_DOTS          3

#define PATTERN_MAIN_COLOR      BLUE
#define PATTERN_SECONDARY_COLOR LIGHT_BLUE

void draw_wallpaper_pattern(void) {
    switch (WALLPAPER_PATTERN) {
        case WALLPAPER_CHECKERBOARD:
            for (int y = 0; y < SCREEN_HEIGHT; y += 10)
                for (int x = 0; x < SCREEN_WIDTH; x += 10) {
                    uint8_t color = ((x/10)+(y/10)) % 2 == 0
                                    ? PATTERN_MAIN_COLOR : PATTERN_SECONDARY_COLOR;
                    fill_rect(x, y, 10, 10, color);
                }
            break;
        case WALLPAPER_STRIPES:
            for (int y = 0; y < SCREEN_HEIGHT; y++) {
                uint8_t color = (y/5) % 2 == 0
                                ? PATTERN_MAIN_COLOR : PATTERN_SECONDARY_COLOR;
                draw_line(0, y, SCREEN_WIDTH-1, y, color);
            }
            break;
        case WALLPAPER_DOTS:
            clear_screen(PATTERN_MAIN_COLOR);
            for (int y = 5; y < SCREEN_HEIGHT; y += 10)
                for (int x = 5; x < SCREEN_WIDTH; x += 10)
                    draw_dot(x, y, PATTERN_SECONDARY_COLOR);
            break;
        default: /* SOLID */
            clear_screen(PATTERN_MAIN_COLOR);
            break;
    }
}

#define ICO_W         20
#define ICO_H         20
#define ICO_LABEL_W    5
#define ICO_LABEL_H    6
#define ICO_CELL_H    34
#define ICO_LABEL_MAX  8

#define DESK_ORIGIN_X  0
#define DESK_ORIGIN_Y  MENUBAR_H

static int  s_drag_idx   = -1;
static int  s_drag_off_x =  0;
static int  s_drag_off_y =  0;

static int  s_last_click_idx  = -1;
static uint32_t s_last_click_tick = 0;
#define DBLCLICK_TICKS 60

typedef struct { const char *name; void (*launch)(void); } app_entry_t;

extern app_descriptor finder_app;
extern app_descriptor clock_app;
extern app_descriptor asmterm_app;
extern app_descriptor monitor_app;

static void launch_finder(void)   { os_launch_app(&finder_app);   }
static void launch_clock(void)    { os_launch_app(&clock_app);    }
static void launch_asmterm(void) { os_launch_app(&asmterm_app); }
static void launch_monitor(void)  { os_launch_app(&monitor_app);  }

static void open_item(desktop_item_t *it) {
    if (it->kind == DESKTOP_ITEM_APP) {
        char app_name[64];
        int ai = 0;
        for (int k = 0; it->name[k] && ai < 63; k++) {
            app_name[ai++] = it->name[k];
        }
        app_name[ai] = '\0';

        char proper[64];
        for (int k = 0; app_name[k]; k++) {
            char c = app_name[k];
            if (k == 0 && c >= 'A' && c <= 'Z') proper[k] = c;
            else if (c >= 'A' && c <= 'Z') proper[k] = c - 'A' + 'a';
            else proper[k] = c;
        }
        proper[ai] = '\0';

        app_descriptor *desc = os_find_app(app_name);

        if (!desc) desc = os_find_app(proper);

        proper[0] = app_name[0];
        if (!desc) {
            for (int k = 0; k < ai; k++) {
                char c = app_name[k];
                proper[k] = (k == 0)
                    ? (c >= 'a' && c <= 'z' ? c-32 : c)
                    : (c >= 'A' && c <= 'Z' ? c+32 : c);
            }
            proper[ai] = '\0';
            desc = os_find_app(proper);
        }

        if (desc) {
            os_launch_app(desc);
        } else {
            char err_msg[256];
            sprintf(err_msg, "Could not find application '%s'.", app_name);
            modal_show(MODAL_ERROR, "App Not Found", err_msg, NULL, NULL);
        }
    } else if (it->kind == DESKTOP_ITEM_DIR) {
        extern void ff_open_dir_pub(uint16_t cluster, const char *path);
        dir_entry_t de;
        uint16_t saved = dir_context.current_cluster;
        dir_context.current_cluster = desktop_fs_cluster();
        bool found = fat16_find(it->name, &de);
        dir_context.current_cluster = saved;
        if (found && (de.attr & ATTR_DIRECTORY)) {
            char path[280];
            sprintf(path, "%s/%s", desktop_fs_path(), it->name);
            ff_open_dir_pub(de.cluster_lo, path);
        }
    }
}


static void draw_file_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax+1, ay,      ICO_W-5, ICO_H,    bg);
    draw_rect(ax+1, ay,      ICO_W-5, ICO_H,    BLACK);
    fill_rect(ax+ICO_W-5, ay, 4, ICO_H,         BLACK);
    draw_line(ax+ICO_W-5, ay, ax+ICO_W-1, ay+4, BLACK);
    fill_rect(ax+ICO_W-4, ay, 3, 4,             bg);
    if (!sel) {
        draw_line(ax+3, ay+5,  ax+ICO_W-6, ay+5,  DARK_GRAY);
        draw_line(ax+3, ay+8,  ax+ICO_W-6, ay+8,  DARK_GRAY);
        draw_line(ax+3, ay+11, ax+ICO_W-6, ay+11, DARK_GRAY);
    }
}

static void draw_folder_icon(int ax, int ay, bool sel) {
    uint8_t bg = sel ? DARK_GRAY : WHITE;
    fill_rect(ax,     ay+3, ICO_W,   ICO_H-3, bg);
    draw_rect(ax,     ay+3, ICO_W,   ICO_H-3, BLACK);
    fill_rect(ax+1,   ay,   8,       4,        bg);
    draw_rect(ax+1,   ay,   8,       4,        BLACK);
    draw_line(ax,     ay+3, ax+1,    ay,       BLACK);
    draw_line(ax+9,   ay,   ax+9,    ay+3,     BLACK);
    if (sel)
        fill_rect(ax+2, ay+5, ICO_W-4, ICO_H-9, BLACK);
}

static void draw_app_icon(int ax, int ay, bool sel) {
    uint8_t bg   = sel ? LIGHT_BLUE : CYAN;
    uint8_t trim = sel ? WHITE : DARK_GRAY;
    fill_rect(ax,   ay,   ICO_W,   ICO_H,   bg);
    draw_rect(ax,   ay,   ICO_W,   ICO_H,   trim);
    /* small "▶" triangle */
    for (int r = 0; r < 7; r++) {
        int w = r + 1;
        fill_rect(ax + 5, ay + 6 + r, w, 1, trim);
    }
}

static void draw_desktop_item(const desktop_item_t *it, bool dragged_ghost) {
    int ax = DESK_ORIGIN_X + it->x;
    int ay = DESK_ORIGIN_Y + it->y;

    if (dragged_ghost) {
        draw_rect(ax, ay, ICO_W, ICO_H, DARK_GRAY);
        return;
    }

    switch (it->kind) {
        case DESKTOP_ITEM_DIR:  draw_folder_icon(ax, ay, it->selected); break;
        case DESKTOP_ITEM_APP:  draw_app_icon   (ax, ay, it->selected); break;
        default:                draw_file_icon  (ax, ay, it->selected); break;
    }

    /* label */
    char disp[ICO_LABEL_MAX + 1];
    int  nlen = (int)strlen(it->name);
    int  dlen = nlen < ICO_LABEL_MAX ? nlen : ICO_LABEL_MAX;
    for (int i = 0; i < dlen; i++) disp[i] = it->name[i];
    disp[dlen] = '\0';

    int lw = dlen * ICO_LABEL_W;
    int lx = ax + ICO_W/2 - lw/2;
    int ly = ay + ICO_H + 2;
    draw_string(lx+1, ly+1, disp, BLACK, 2);
    draw_string(lx,   ly,   disp, WHITE, 2);
}

static int desktop_hit(int mx, int my) {
    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();
    for (int i = 0; i < count; i++) {
        if (!items[i].used) continue;
        int ax = DESK_ORIGIN_X + items[i].x;
        int ay = DESK_ORIGIN_Y + items[i].y;
        if (mx >= ax && mx < ax + ICO_W &&
            my >= ay && my < ay + ICO_H + ICO_LABEL_H + 2)
            return i;
    }
    return -1;
}

static bool over_any_window(int mx, int my) {
    for (int i = 0; i < win_count; i++) {
        window *w = win_stack[i];
        if (!w->visible || w->minimized || w->pinned_bottom) continue;
        int wy = w->y + MENUBAR_H;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= wy    && my < wy  + w->h)
            return true;
    }
    return false;
}

bool desktop_accept_drop(const char *src_path, const char *src_name) {
    if (!src_path || !src_name) return false;

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = desktop_fs_cluster();

    dir_entry_t de;
    if (fat16_find(src_name, &de)) {
        dir_context.current_cluster = saved;
        return false;
    }

    dir_entry_t src_de;
    dir_context.current_cluster = saved;
    bool found = fat16_find(src_path, &src_de);
    if (!found) return false;

    dir_context.current_cluster = desktop_fs_cluster();
    bool ok;
    if (src_de.attr & ATTR_DIRECTORY)
        ok = fat16_move_dir(src_path, src_name);
    else
        ok = fat16_move_file(src_path, src_name);

    dir_context.current_cluster = saved;

    if (ok) desktop_fs_set_dirty();
    return ok;
}

static menu *s_apps_menu;

static void menu_launch_finder(void)   { launch_finder();   }
static void menu_launch_clock(void)    { launch_clock();    }
static void menu_launch_asmterm(void) { launch_asmterm(); }
static void menu_launch_monitor(void)  { launch_monitor();  }


void desktop_init(void) {
    int desk_y = MENUBAR_H;
    int desk_h = SCREEN_HEIGHT - MENUBAR_H - TASKBAR_H;

    static const window_spec_t spec = {
        .x = 0, .y = 0,
        .w = SCREEN_WIDTH,
        .h = SCREEN_HEIGHT - MENUBAR_H - TASKBAR_H,
        .title = "", .visible = true,
    };
    window *win = wm_register(&spec);
    if (!win) return;
    win->visible_buttons = false;
    win->pinned_bottom   = true;
    win->show_order      = -1;

    s_apps_menu = window_add_menu(win, "Apps");
    menu_add_item(s_apps_menu, "Finder",   menu_launch_finder);
    menu_add_item(s_apps_menu, "Clock",    menu_launch_clock);
    menu_add_item(s_apps_menu, "ASMTerm", menu_launch_asmterm);
    menu_add_item(s_apps_menu, "Monitor",  menu_launch_monitor);

    (void)desk_y; (void)desk_h;

    desktop_fs_init();
}

static bool any_window_captured(void) {
    for (int i = 0; i < win_count; i++) {
        if (win_stack[i]->dragging || win_stack[i]->resizing) return true;
    }
    return false;
}

static void draw(int count, desktop_item_t *items) {
	for (int i = 0; i < count; i++) {
        if (!items[i].used) continue;
        bool is_being_dragged = (i == s_drag_idx);
        if (!is_being_dragged)
            draw_desktop_item(&items[i], false);
    }

    if (s_drag_idx >= 0 && s_drag_idx < count)
        draw_desktop_item(&items[s_drag_idx], false);
}

void desktop_on_frame(void) {
    draw_wallpaper_pattern();

    if (desktop_fs_is_dirty())
        desktop_fs_reload();

    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();

    bool blocked = any_window_captured();

    if (s_drag_idx >= 0) {
        desktop_item_t *it = &items[s_drag_idx];

        if (mouse.left) {
            int new_x = mouse.x - DESK_ORIGIN_X - s_drag_off_x;
            int new_y = mouse.y - DESK_ORIGIN_Y - s_drag_off_y;
            if (new_x < 0) new_x = 0;
            if (new_y < 0) new_y = 0;
            if (new_x > SCREEN_WIDTH  - ICO_W) new_x = SCREEN_WIDTH  - ICO_W;
            if (new_y > SCREEN_HEIGHT - MENUBAR_H - ICO_H - ICO_LABEL_H - TASKBAR_H)
                new_y = SCREEN_HEIGHT - MENUBAR_H - ICO_H - ICO_LABEL_H - TASKBAR_H;
            it->x = new_x;
            it->y = new_y;
        } else {
            desktop_fs_move_icon(s_drag_idx, it->x, it->y);
            s_drag_idx = -1;
        }
    }

    if (mouse.left && !mouse.left_clicked && s_drag_idx < 0) {
        for (int i = 0; i < count; i++) {
            if (!items[i].selected || !items[i].used) continue;
            int ax = DESK_ORIGIN_X + items[i].x;
            int ay = DESK_ORIGIN_Y + items[i].y;
            if (mouse.x >= ax && mouse.x < ax + ICO_W &&
                mouse.y >= ay && mouse.y < ay + ICO_H) {
                if (mouse.dx != 0 || mouse.dy != 0) {
                    s_drag_off_x = mouse.x - ax;
                    s_drag_off_y = mouse.y - ay;
                    s_drag_idx   = i;
                    s_last_click_idx = -1;
                }
                break;
            }
        }
    }

    if (mouse.left_clicked) {
        int hit = desktop_hit(mouse.x, mouse.y);
        if (hit >= 0) {
            for (int i = 0; i < count; i++) items[i].selected = false;
            items[hit].selected = true;

            uint32_t now = pit_ticks;
            if (hit == s_last_click_idx &&
                (now - s_last_click_tick) <= DBLCLICK_TICKS) {
                open_item(&items[hit]);
                s_last_click_idx  = -1;
                s_last_click_tick = 0;
            } else {
                s_last_click_idx  = hit;
                s_last_click_tick = now;
            }
        } else {
            for (int i = 0; i < count; i++) items[i].selected = false;
            s_last_click_idx = -1;
        }
    }

    draw(count, items);
}
