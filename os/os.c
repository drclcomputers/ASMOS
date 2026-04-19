#include "os/os.h"
#include "os/app_registry.h"
#include "os/error.h"
#include "os/scheduler.h"

#include "lib/device.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

#include "ui/modal.h"
#include "ui/ui.h"
#include "ui/window.h"

#include "config/config.h"
#include "io/ps2.h"

menubar g_menubar;
bool g_menubar_click_consumed = false;
bool gui_should_exit = false;

app_descriptor *installed_apps[MAX_INSTALLED_APPS];
int installed_app_count = 0;

app_instance_t running_apps[MAX_RUNNING_APPS];
int running_app_count = 0;

void os_install_app(app_descriptor *desc) {
    if (!desc) {
        ERR_WARN_REPORT(ERR_NULL_PTR, "os_install_app");
        return;
    }
    if (installed_app_count >= MAX_INSTALLED_APPS) {
        ERR_WARN_REPORT(ERR_APP_MAX_RUNNING, "os_install_app: registry full");
        return;
    }
    installed_apps[installed_app_count++] = desc;
}

app_instance_t *os_launch_app(app_descriptor *desc) {
    if (!desc) {
        ERR_WARN_REPORT(ERR_NULL_PTR, "os_launch_app");
        return NULL;
    }
    if (running_app_count >= MAX_RUNNING_APPS) {
        ERR_WARN_REPORT(ERR_APP_MAX_RUNNING, "os_launch_app");
        return NULL;
    }

    int slot = -1;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (!running_apps[i].running) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ERR_WARN_REPORT(ERR_APP_MAX_RUNNING, "os_launch_app: no slot");
        return NULL;
    }

    app_instance_t *app = &running_apps[slot];
    app->desc = desc;
    app->task_slot = -1;
    app->wants_quit = false;

    if (desc->state_size > 0) {
        app->state = kmalloc(desc->state_size);
        if (!app->state) {
            ERR_WARN_REPORT(ERR_APP_ALLOC, desc->name ? desc->name : "?");
            return NULL;
        }
        memset(app->state, 0, desc->state_size);
    } else {
        app->state = NULL;
    }

    app->running = true;
    running_app_count++;

    if (desc->init)
        desc->init(app->state);

    return app;
}

void os_quit_app(app_instance_t *inst) {
    if (inst && inst->running)
        inst->wants_quit = true;
}

void os_quit_app_by_desc(app_descriptor *desc) {
    if (!desc)
        return;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (running_apps[i].running && running_apps[i].desc == desc) {
            running_apps[i].wants_quit = true;
        }
    }
}

app_instance_t *os_find_instance(app_descriptor *desc) {
    if (!desc)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++)
        if (running_apps[i].running && running_apps[i].desc == desc &&
            !running_apps[i].wants_quit)
            return &running_apps[i];
    return NULL;
}

int os_find_instances(app_descriptor *desc, app_instance_t **out, int max) {
    if (!desc || !out)
        return 0;
    int found = 0;
    for (int i = 0; i < MAX_RUNNING_APPS && found < max; i++)
        if (running_apps[i].running && running_apps[i].desc == desc &&
            !running_apps[i].wants_quit)
            out[found++] = &running_apps[i];
    return found;
}

app_descriptor *os_find_app(const char *name) {
    if (!name)
        return NULL;
    for (int i = 0; i < installed_app_count; i++)
        if (strcmp(installed_apps[i]->name, name) == 0)
            return installed_apps[i];
    return NULL;
}

void os_request_exit(void) { gui_should_exit = true; }

void os_tick_apps(void) {
    uint32_t now = time_millis();
    static uint32_t last_run_time[MAX_RUNNING_APPS];

    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *app = &running_apps[i];
        if (!app->running || app->wants_quit)
            continue;

        if (now - last_run_time[i] >= FRAME_TIME_MS) {
            if (app->desc && app->desc->on_frame) {
                app->desc->on_frame(app->state);
            }
            last_run_time[i] = now;
        }
    }
}

void os_reap_dead_apps(void) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *app = &running_apps[i];
        if (!app->running || !app->wants_quit)
            continue;

        if (app->desc && app->desc->destroy)
            app->desc->destroy(app->state);

        if (app->state) {
            kfree(app->state);
            app->state = NULL;
        }

        if (app->task_slot >= 1)
            scheduler_kill_task(app->task_slot);

        app->running = false;
        app->wants_quit = false;
        app->desc = NULL;
        app->task_slot = -1;
        running_app_count--;

        for (int w = win_count - 1; w >= 0; w--) {
            if (win_stack[w]->visible && !win_stack[w]->minimized) {
                wm_focus(win_stack[w]);
                break;
            }
        }
    }

    scheduler_reap();
}
