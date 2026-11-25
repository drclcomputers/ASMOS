// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/gpu.h"
#include "os/api.h"
#include "os/appload.h"
#include "os/meminfo.h"

#define REFRESH_MS 1000

typedef struct {
    window *win; /* must stay first */
    control *tabs, *list, *sw, *end;
    control *cpu, *mem, *up, *memmeter;
    control *drive_lbl[4], *drive_bar[4];
    uint32_t next_refresh;
    char sig[64]; /* detects changes in the task list */
} monitor_state_t;

app_descriptor monitor_app;

static void fill_tasks(monitor_state_t *s) {
    char sig[64];
    int n = 0;
    for (int i = 0; i < MAX_RUNNING_APPS && n < 63; i++)
        sig[n++] = (running_apps[i].running && !running_apps[i].wants_quit)
                       ? (running_apps[i].suspended ? 'a' : 'A') + (i % 26)
                       : '.';
    sig[n] = 0;
    if (strcmp(sig, s->sig) == 0)
        return;
    strcpy(s->sig, sig);

    uint32_t keep = list_data(s->list, list_sel(s->list));
    if (g_os_stopped_app >= 0) { /* just stopped by the watchdog: show it */
        keep = (uint32_t)g_os_stopped_app;
        g_os_stopped_app = -1;
    }
    list_clear(s->list);
    list_add(s->list, "App\tKind\tWindow");
    int pick = 1;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->wants_quit || !a->desc)
            continue;
        window *w = os_instance_window(a);
        char row[80];
        snprintf(row, sizeof(row), "%s\t%s\t%s", a->desc->name,
                 appload_find(a->desc->name) ? ".APP" : "built-in",
                 a->suspended   ? "NOT RESPONDING"
                 : !w           ? "-"
                 : w->minimized ? "minimized"
                                : "open");
        int idx = list_add_data(s->list, row, (uint32_t)i);
        if ((uint32_t)i == keep)
            pick = idx;
    }
    list_set_sel(s->list, pick);
}

static void refresh_system(monitor_state_t *s) {
    char b[80];
    /* the RAM there is to use (not video memory and the BIOS: never free) */
    meminfo_t mi;
    meminfo(&mi);
    uint32_t usable = (mi.total - mi.reserved) / 1024;
    uint32_t used = usable - mi.free / 1024;
    snprintf(b, sizeof(b), "Memory: %u KB used of %u KB", used, usable);
    ctl_set_text(s->mem, b);
    ctl_set_range(s->memmeter, 0, usable ? usable : 1, 0);
    ctl_set_value(s->memmeter, used);
    char tip[160];
    snprintf(tip, sizeof(tip),
             "System %u KB, screen %u KB, apps %u KB, free %u KB. "
             "(%u KB more: video memory and the BIOS)",
             (mi.kernel + mi.heap) / 1024, mi.screen / 1024, mi.apps / 1024,
             mi.free / 1024, mi.reserved / 1024);
    ctl_set_tooltip(s->mem, tip);
    ctl_set_tooltip(s->memmeter, tip);
    time_hms_t u = uptime_hms();
    snprintf(b, sizeof(b), "Up:   %02u:%02u:%02u    Display: %u fps", u.hours,
             u.minutes, u.seconds, gpu_fps());
    ctl_set_text(s->up, b);

    int line = 0;
    for (int d = 0; d < DRIVE_COUNT && line < 4; d++) {
        if (!fs_drive_mounted(d))
            continue;
        uint32_t tot = 0, usedb = 0;
        if (fs_get_usage_drive(d, &tot, &usedb))
            snprintf(b, sizeof(b), "%s (%s): %u / %u KB", fs_drive_label(d),
                     g_drive_paths[d], usedb / 1024, tot / 1024);
        else
            snprintf(b, sizeof(b), "%s: unavailable", fs_drive_label(d));
        ctl_set_text(s->drive_lbl[line], b);
        ctl_set_range(s->drive_bar[line], 0, tot ? tot / 1024 : 1, 0);
        ctl_set_value(s->drive_bar[line], usedb / 1024);
        ctl_show(s->drive_lbl[line], true);
        ctl_show(s->drive_bar[line], true);
        line++;
    }
    for (; line < 4; line++) {
        ctl_show(s->drive_lbl[line], false);
        ctl_show(s->drive_bar[line], false);
    }
}

static app_instance_t *selected_app(monitor_state_t *s) {
    int sel = list_sel(s->list);
    if (sel < 1)
        return NULL;
    app_instance_t *a = &running_apps[list_data(s->list, sel)];
    return (a->running && !a->wants_quit) ? a : NULL;
}

static void do_switch(monitor_state_t *s) {
    app_instance_t *a = selected_app(s);
    if (a)
        a->suspended = false; /* give a stopped app another go */
    window *w = a ? os_instance_window(a) : NULL;
    if (!w)
        return;
    w->minimized = false;
    w->visible = true;
    wm_focus(w);
}

static void do_end(monitor_state_t *s) {
    app_instance_t *a = selected_app(s);
    if (!a || a->desc == &monitor_app)
        return;
    window *w = os_instance_window(a);
    if (w && !a->suspended)
        wm_close(w); /* lets the app ask "save changes?" etc. */
    else
        os_quit_app(a); /* not responding: none of its code runs */
}

static void on_button(control *c, int ev) {
    (void)ev;
    if (c->id == 1)
        do_switch(c->user);
    else
        do_end(c->user);
}

static void on_list(control *c, int ev) {
    if (ev == EV_SUBMIT)
        do_switch(c->user);
}

static void on_refresh(void) {
    monitor_state_t *s = ui_active_state();
    if (s) {
        s->sig[0] = 0;
        fill_tasks(s);
        refresh_system(s);
    }
}
static void on_close(void) { os_quit_app_by_desc(&monitor_app); }
static void on_about(void) {
    dlg_message(NULL, "About Monitor",
                "Monitor v3.0\nTask list & system stats\n"
                "Ctrl+Shift+Esc opens it from anywhere.");
}

static void monitor_init(void *state) {
    monitor_state_t *s = state;
    s->win = app_window(s, "Monitor", 20, 20, 220, 148, DARK_GRAY, 0);
    if (!s->win)
        return;
    window *w = s->win;
    s->tabs = ctl_tabs(w, 4, 3, 210, "Tasks|System", NULL);

    s->list = ctl_list(w, 4, 20, 210, 92, LBS_HEADER, on_list);
    int cols[3] = {80, 60, 0};
    list_set_columns(s->list, cols, 3);
    s->sw = ctl_button(w, 104, 116, 54, 13, "Switch To", 1, on_button);
    s->end = ctl_button(w, 162, 116, 52, 13, "End Task", 2, on_button);
    ctl_style_add(s->sw, CS_DEFAULT);
    ctl_on_page(s->list, s->tabs, 0);
    ctl_on_page(s->sw, s->tabs, 0);
    ctl_on_page(s->end, s->tabs, 0);

    s->cpu = ctl_label(w, 8, 22, 204, 8, "", 0);
    s->mem = ctl_label(w, 8, 32, 204, 8, "", 0);
    s->memmeter = ctl_progress(w, 8, 41, 204, 6, 1);
    s->up = ctl_label(w, 8, 50, 204, 8, "", 0);
    control *g = ctl_group(w, 4, 60, 212, 70, "Drives");
    ctl_on_page(s->cpu, s->tabs, 1);
    ctl_on_page(s->mem, s->tabs, 1);
    ctl_on_page(s->memmeter, s->tabs, 1);
    ctl_on_page(s->up, s->tabs, 1);
    ctl_on_page(g, s->tabs, 1);
    for (int i = 0; i < 4; i++) {
        s->drive_lbl[i] = ctl_label(w, 10, 70 + i * 15, 200, 8, "", 0);
        s->drive_bar[i] = ctl_progress(w, 10, 78 + i * 15, 200, 5, 1);
        ctl_on_page(s->drive_lbl[i], s->tabs, 1);
        ctl_on_page(s->drive_bar[i], s->tabs, 1);
    }

    menu *f = window_add_menu(w, "File");
    menu_add_item(f, "Refresh", on_refresh);
    menu_add_separator(f);
    menu_add_item(f, "Close", on_close);
    menu_add_separator(f);
    menu_add_item(f, "About Monitor", on_about);

    char b[64];
    cpu_model_init(); /* CPUID once; the name doesn't change */
    snprintf(b, sizeof(b), "CPU:  %s", cpu_model_str());
    ctl_set_text(s->cpu, b);
    fill_tasks(s);
    refresh_system(s);
    ctl_focus(s->list);
}

static void monitor_on_frame(void *state) {
    monitor_state_t *s = state;
    fill_tasks(s);
    /* System page only while it is showing; disk usage is cached by fs */
    if (ctl_value(s->tabs) == 1 && time_millis() >= s->next_refresh) {
        s->next_refresh = time_millis() + REFRESH_MS;
        refresh_system(s);
    }
    ctl_enable(s->end,
               selected_app(s) && selected_app(s)->desc != &monitor_app);
}

app_descriptor monitor_app = {
    .name = "MONITOR",
    .state_size = sizeof(monitor_state_t),
    .init = monitor_init,
    .on_frame = monitor_on_frame,
    .single_instance = true,
};
