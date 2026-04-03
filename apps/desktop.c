#include "os/api.h"

typedef struct {
    window *win;
} desktop_state_t;

app_descriptor desktop_app;

static void launch_finder(void) {
    extern app_descriptor finder_app;
    os_launch_app(&finder_app);
}

static void launch_terminal(void) {
    extern app_descriptor terminal_app;
    os_launch_app(&terminal_app);
}

static void launch_monitor(void) {
    extern app_descriptor monitor_app;
    os_launch_app(&monitor_app);
}

static void desktop_init(void *state) {
    desktop_state_t *s = (desktop_state_t *)state;

    const window_spec_t spec = {
        .x             = 0,
        .y             = 0,
        .w             = 1,
        .h             = 1,
        .title         = "",
        .title_color   = 0,
        .bar_color     = 0,
        .content_color = 0,
        .visible       = true,
        .on_close      = NULL,
    };
    s->win = wm_register(&spec);
    s->win->visible_buttons = false;
    if (!s->win) return;

    menu *apps_menu = window_add_menu(s->win, "Apps");
    menu_add_item(apps_menu, "Finder", launch_finder);
    menu_add_item(apps_menu, "Terminal", launch_terminal);
    menu_add_item(apps_menu, "Monitor", launch_monitor);
}

static void desktop_on_frame(void *state) {
    desktop_state_t *s = (desktop_state_t *)state;

    bool any_visible = false;
    for (int i = 0; i < win_count; i++) {
        if (win_stack[i] != s->win && win_stack[i]->visible && !win_stack[i]->minimized) {
            any_visible = true;
            break;
        }
    }

    if (!any_visible) {
        wm_focus(s->win);
    }
}

static void desktop_destroy(void *state) {
    desktop_state_t *s = (desktop_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor desktop_app = {
    .name       = "Desktop",
    .state_size = sizeof(desktop_state_t),
    .init       = desktop_init,
    .on_frame   = desktop_on_frame,
    .destroy    = desktop_destroy,
};
