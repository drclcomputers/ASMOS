#include "os/api.h"

#define UPDATE_INTERVAL 60

#define WIN_W 210
#define WIN_H 115

#define TAB_H 12
#define TAB_W 80
#define CONTENT_X 6
#define CONTENT_Y (TAB_H)

#define LIST_PAD 4
#define LIST_ITEM_H 10
#define LIST_VISIBLE 5
#define LIST_H (LIST_VISIBLE * LIST_ITEM_H)
#define LIST_Y (CONTENT_Y + LIST_PAD + 4)
#define LIST_W (WIN_W - 14)

#define BTN_W (WIN_W - 2 * LIST_PAD - 3 * 9) / 3
#define BTN_H 12
#define BTN_Y (LIST_Y + LIST_H + 6)

typedef struct {
    window *win;
    int tab;
    int selected;

    char cpu_str[64];
    char mem_str[64];
    char drive_str[4][64];
    int drive_count;
    uint32_t frame_counter;
} monitor_state_t;

app_descriptor monitor_app;

static monitor_state_t *active_monitor(void) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &monitor_app)
            continue;
        return (monitor_state_t *)a->state;
    }
    return NULL;
}

static void fmt_bytes(char *dst, int dst_size, uint32_t used, uint32_t total) {
    char u[12], t[12];
    uint32_to_str(used, u);
    uint32_to_str(total, t);
    int pos = 0;
    for (int i = 0; u[i] && pos < dst_size - 1; i++)
        dst[pos++] = u[i];
    const char *sep = " KB / ";
    for (int i = 0; sep[i] && pos < dst_size - 1; i++)
        dst[pos++] = sep[i];
    for (int i = 0; t[i] && pos < dst_size - 1; i++)
        dst[pos++] = t[i];
    const char *unit = " KB";
    for (int i = 0; unit[i] && pos < dst_size - 1; i++)
        dst[pos++] = unit[i];
    dst[pos] = '\0';
}

static void refresh_cpu(monitor_state_t *s) {
    cpu_model_init();
    strcpy(s->cpu_str, cpu_model_str());
}

static void refresh_ram(monitor_state_t *s) {
    uint32_t mem_used = heap_used() / 1024;
    uint32_t mem_total = (heap_used() + heap_remaining()) / 1024;
    char tmp[52];
    fmt_bytes(tmp, sizeof(tmp), mem_used, mem_total);
    snprintf(s->mem_str, sizeof(s->mem_str), "Memory: %s", tmp);
}

static void refresh_drives(monitor_state_t *s) {
    int line = 0;
    for (int d = 0; d < DRIVE_COUNT; d++) {
        if (!fs_drive_mounted(d))
            continue;
        uint32_t tot = 0, used = 0;
        if (fs_get_usage_drive(d, &tot, &used)) {
            used /= 1024;
            tot /= 1024;
            const char *label = fs_drive_label(d);
            const char *vpath = (d == DRIVE_HDA)    ? "/HDA"
                                : (d == DRIVE_HDB)  ? "/HDB"
                                : (d == DRIVE_FDD0) ? "/FDD0"
                                                    : "/FDD1";
            char tmp[52];
            int llen = strlen(label);
            int vlen = strlen(vpath);
            int pos = 0;
            for (int i = 0; i < llen && pos < 63; i++)
                s->drive_str[line][pos++] = label[i];
            s->drive_str[line][pos++] = ' ';
            s->drive_str[line][pos++] = '(';
            for (int i = 0; i < vlen && pos < 63; i++)
                s->drive_str[line][pos++] = vpath[i];
            s->drive_str[line][pos++] = ')';
            s->drive_str[line][pos++] = ':';
            s->drive_str[line][pos++] = ' ';
            s->drive_str[line][pos] = '\0';
            fmt_bytes(tmp, sizeof(tmp), used, tot);
            int tlen = strlen(tmp);
            for (int i = 0; i < tlen && pos < 63; i++)
                s->drive_str[line][pos++] = tmp[i];
            s->drive_str[line][pos] = '\0';
        } else {
            snprintf(s->drive_str[line], sizeof(s->drive_str[0]),
                     "%s: unavailable", fs_drive_label(d));
        }
        if (++line >= 4)
            break;
    }
    s->drive_count = line;
}

static int count_running(void) {
    int n = 0;
    for (int i = 0; i < MAX_RUNNING_APPS; i++)
        if (running_apps[i].running && !running_apps[i].wants_quit)
            n++;
    return n;
}

static app_instance_t *get_nth(int n) {
    int idx = 0;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->wants_quit)
            continue;
        if (idx == n)
            return a;
        idx++;
    }
    return NULL;
}

static void do_switch(void) {
    monitor_state_t *s = active_monitor();
    if (!s || s->selected < 0)
        return;
    app_instance_t *a = get_nth(s->selected);
    if (!a || !a->state)
        return;
    window **wp = (window **)a->state;
    if (*wp) {
        (*wp)->minimized = false;
        wm_focus(*wp);
    }
}

static void do_end_task(void) {
    monitor_state_t *s = active_monitor();
    if (!s || s->selected < 0)
        return;
    app_instance_t *a = get_nth(s->selected);
    if (!a || a->desc == &monitor_app)
        return;
    if (a->state) {
        window **wp = (window **)a->state;
        if (*wp && (*wp)->on_close)
            (*wp)->on_close(*wp);
        else
            os_quit_app(a);
    } else {
        os_quit_app(a);
    }
    s->selected = -1;
}

static void do_cancel(void) {
    monitor_state_t *s = active_monitor();
    if (s)
        s->selected = -1;
}

static void menu_refresh(void) {
    monitor_state_t *s = active_monitor();
    if (!s)
        return;
    refresh_ram(s);
    refresh_drives(s);
}

static bool monitor_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&monitor_app);
    return true;
}

static void on_file_close(void) { monitor_close(NULL); }

static void on_about(void) {
    modal_show(MODAL_INFO, "About Monitor",
               "Monitor v2.0\nTask List & System Stats", NULL, NULL);
}

static void draw_tab(int tx, int ty, int idx, int active, const char *label) {
    uint8_t bg = (idx == active) ? LIGHT_GRAY : DARK_GRAY;
    uint8_t fg = (idx == active) ? BLACK : WHITE;
    fill_rect(tx, ty, TAB_W, TAB_H, bg);
    draw_rect(tx, ty, TAB_W, TAB_H, BLACK);

    int tw = (int)strlen(label) * 5;
    draw_string(tx + (TAB_W - tw) / 2, ty + 3, (char *)label, fg, 2);
}

static void draw_btn(int x, int y, const char *label) {
    fill_rect(x + 2, y + 2, BTN_W, BTN_H, BLACK);
    fill_rect(x, y, BTN_W, BTN_H, LIGHT_GRAY);
    draw_rect(x, y, BTN_W, BTN_H, BLACK);
    draw_line(x + 1, y + 1, x + BTN_W - 2, y + 1, WHITE);
    draw_line(x + 1, y + 1, x + 1, y + BTN_H - 2, WHITE);
    int tw = (int)strlen(label) * 5;
    draw_string(x + (BTN_W - tw) / 2, y + (BTN_H - 6) / 2, (char *)label, BLACK,
                2);
}

static bool btn_hit(int x, int y) {
    return mouse.left_clicked && mouse.x >= x && mouse.x < x + BTN_W &&
           mouse.y >= y && mouse.y < y + BTN_H;
}

static void monitor_draw(window *win, void *ud) {
    monitor_state_t *s = (monitor_state_t *)ud;
    if (!s)
        return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;

    fill_rect(wx, wy, win->w - 2, win->h - 16, LIGHT_GRAY);

    draw_tab(wx + CONTENT_X + LIST_PAD, wy, 0, s->tab, "Task List");
    draw_tab(wx + CONTENT_X + LIST_PAD + TAB_W + 2, wy, 1, s->tab, "System");

    int panel_x = wx + CONTENT_X;
    int panel_y = wy + CONTENT_Y;
    int panel_w = win->w - 2 - CONTENT_X * 2;
    int panel_h = win->h - TITLEBAR_H - CONTENT_Y;
    fill_rect(panel_x + 1, panel_y + 1, panel_w - 2, panel_h - 2, LIGHT_GRAY);

    if (s->tab == 0) {
        int lx = panel_x + LIST_PAD;
        int ly = wy + LIST_Y;
        int lw = panel_w - LIST_PAD * 2;

        fill_rect(lx, ly, lw, LIST_H, WHITE);
        draw_rect(lx, ly, lw, LIST_H, BLACK);

        int count = count_running();
        for (int i = 0; i < LIST_VISIBLE && i < count; i++) {
            app_instance_t *a = get_nth(i);
            if (!a || !a->desc)
                continue;
            int iy = ly + i * LIST_ITEM_H;
            if (i == s->selected) {
                fill_rect(lx + 1, iy, lw - 2, LIST_ITEM_H, DARK_GRAY);
                draw_string(lx + 4, iy + 2, (char *)a->desc->name, WHITE, 2);
            } else {
                draw_string(lx + 4, iy + 2, (char *)a->desc->name, BLACK, 2);
            }
        }

        int b1x = lx;
        int b2x = b1x + BTN_W + 6;
        int b3x = b2x + BTN_W + 6;
        int by = wy + BTN_Y;
        draw_btn(b1x, by, "Switch To");
        draw_btn(b2x, by, "End Task");
        draw_btn(b3x, by, "Cancel");
    } else {
        int sy = panel_y + 6;
        draw_string(panel_x, sy, s->cpu_str, BLACK, 2);
        sy += 10;
        draw_string(panel_x, sy, s->mem_str, BLACK, 2);
        sy += 10;
        draw_line(panel_x - 2, sy, panel_x + panel_w - 4, sy, DARK_GRAY);
        sy += 6;
        draw_string(panel_x, sy, "Drives:", BLACK, 2);
        sy += 10;
        for (int i = 0; i < s->drive_count; i++) {
            draw_string(panel_x + 4, sy, s->drive_str[i], BLACK, 2);
            sy += 10;
        }
    }
}

static void monitor_init(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;
    s->selected = -1;
    s->tab = 0;

    int drives = 0;
    for (int d = 0; d < DRIVE_COUNT; d++)
        if (fs_drive_mounted(d))
            drives++;

    const window_spec_t spec = {
        .x = 20,
        .y = 20,
        .w = WIN_W,
        .h = WIN_H,
        .title = "Monitor",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = LIGHT_GRAY,
        .visible = true,
        .on_close = monitor_close,
        .resizable = false,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;

    s->win->on_draw = monitor_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Refresh", menu_refresh);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", on_file_close);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "About Monitor", on_about);

    refresh_cpu(s);
    refresh_ram(s);
    refresh_drives(s);
}

static void monitor_on_frame(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;
    if (!s->win)
        return;

    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16;

    int t0x = wx + CONTENT_X;
    int t1x = t0x + TAB_W + 2;
    if (mouse.left_clicked && mouse.y >= wy && mouse.y < wy + TAB_H) {
        if (mouse.x >= t0x && mouse.x < t0x + TAB_W) {
            s->tab = 0;
            return;
        }
        if (mouse.x >= t1x && mouse.x < t1x + TAB_W) {
            s->tab = 1;
            return;
        }
    }

    if (s->tab == 0) {
        int panel_x = wx + CONTENT_X;
        int panel_w = s->win->w - 2 - CONTENT_X * 2;
        int lx = panel_x + LIST_PAD;
        int ly = wy + LIST_Y;
        int lw = panel_w - LIST_PAD * 2;

        if (mouse.left_clicked && mouse.x >= lx && mouse.x < lx + lw &&
            mouse.y >= ly && mouse.y < ly + LIST_H) {
            int row = (mouse.y - ly) / LIST_ITEM_H;
            int count = count_running();
            if (row >= 0 && row < count)
                s->selected = row;
        }

        int b1x = lx;
        int b2x = b1x + BTN_W + 6;
        int b3x = b2x + BTN_W + 6;
        int by = wy + BTN_Y;
        if (btn_hit(b1x, by))
            do_switch();
        else if (btn_hit(b2x, by))
            do_end_task();
        else if (btn_hit(b3x, by))
            do_cancel();
    }

    s->frame_counter++;
    if (s->frame_counter >= UPDATE_INTERVAL) {
        s->frame_counter = 0;
        refresh_ram(s);
    }
}

static void monitor_destroy(void *state) {
    monitor_state_t *s = (monitor_state_t *)state;
    wm_unregister(s->win);
    s->win = NULL;
}

app_descriptor monitor_app = {
    .name = "MONITOR",
    .state_size = sizeof(monitor_state_t),
    .init = monitor_init,
    .on_frame = monitor_on_frame,
    .destroy = monitor_destroy,
    .single_instance = true,
};
