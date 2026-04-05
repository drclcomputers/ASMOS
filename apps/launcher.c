#include "os/api.h"
#include "ui/icons.h"

extern app_descriptor finder_app;
extern app_descriptor terminal_app;
extern app_descriptor monitor_app;

typedef struct {
    window      *win;
    icon_view_t  icons;
} launcher_state_t;

app_descriptor launcher_app;

static void sync_icon_origin(launcher_state_t *s) {
    int ox = s->win->x + 1;
    int oy = s->win->y + MENUBAR_H + 17;
    int aw = s->win->w - 2;
    int ah = s->win->h - 17 - 1;
    icon_view_set_origin(&s->icons, ox, oy, aw, ah);
}

static bool launcher_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&launcher_app);
    return true;
}

static void on_file_close(void) { launcher_close(NULL); }

static void launch_finder(void)   { os_launch_app(&finder_app); }
static void launch_terminal(void) { os_launch_app(&terminal_app); }
static void launch_monitor(void)  { os_launch_app(&monitor_app); }

static void launcher_init(void *state) {
    launcher_state_t *s = (launcher_state_t *)state;

    const window_spec_t spec = {
        .x             = 40,
        .y             = 30,
        .w             = 160,
        .h             = 120,
        .resizable     = false,
        .title         = "Launcher",
        .title_color   = WHITE,
        .bar_color     = DARK_GRAY,
        .content_color = BLUE,
        .visible       = true,
        .on_close      = launcher_close,
    };
    s->win = wm_register(&spec);
    if (!s->win) return;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);

    icon_view_init(&s->icons, 0, 0, 1, 1);
    sync_icon_origin(s);

    icon_view_add(&s->icons, "Terminal", launch_terminal, -1, -1);
    icon_view_add(&s->icons, "Finder",   launch_finder,   -1, -1);
    icon_view_add(&s->icons, "Monitor",  launch_monitor,  -1, -1);
}

static void launcher_on_frame(void *state) {
    launcher_state_t *s = (launcher_state_t *)state;
    if (!s->win || !s->win->visible) return;

    sync_icon_origin(s);

    bool blocked = s->win->dragging || !s->win->visible;
    icon_view_update(&s->icons, blocked);
    icon_view_draw(&s->icons);
}

static void launcher_destroy(void *state) {
    launcher_state_t *s = (launcher_state_t *)state;
    if (s->win) { wm_unregister(s->win); s->win = NULL; }
}

app_descriptor launcher_app = {
    .name       = "Launcher",
    .state_size = sizeof(launcher_state_t),
    .init       = launcher_init,
    .on_frame   = launcher_on_frame,
    .destroy    = launcher_destroy,
};
