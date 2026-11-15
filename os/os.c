#include "os/os.h"
#include "os/app_registry.h"
#include "os/appguard.h"
#include "os/appload.h"
#include "os/error.h"
#include "os/scheduler.h"

#include "drivers/serial.h"
#include "drivers/speaker.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

#include "ui/ui.h"
#include "ui/window.h"

#include "config/config.h"
#include "drivers/ps2.h"

menubar g_menubar;
bool g_menubar_click_consumed = false;
bool gui_should_exit = false;

app_descriptor *installed_apps[MAX_INSTALLED_APPS];
int installed_app_count = 0;

app_instance_t running_apps[MAX_RUNNING_APPS];
int running_app_count = 0;

global_alarm_t g_alarm = {0};

void os_install_app(app_descriptor *desc) {
    if (!desc) {
        ERR_WARN_REPORT(ERR_NULL_PTR, "os_install_app");
        return;
    }
    if (installed_app_count >= MAX_INSTALLED_APPS) {
        ERR_WARN_REPORT(ERR_APP_MAX_RUNNING, "os_install_app: registry full");
        return;
    }
    for (int i = 0; i < installed_app_count; i++)
        if (installed_apps[i] == desc)
            return; /* already installed */
    installed_apps[installed_app_count++] = desc;
}

void os_uninstall_app(app_descriptor *desc) {
    if (!desc)
        return;
    for (int i = 0; i < installed_app_count; i++) {
        if (installed_apps[i] != desc)
            continue;
        for (int j = i; j < installed_app_count - 1; j++)
            installed_apps[j] = installed_apps[j + 1];
        installed_apps[--installed_app_count] = NULL;
        return;
    }
}

window *os_instance_window(const app_instance_t *a) {
    if (!a || !a->state || !a->desc || a->desc->opaque_state)
        return NULL;
    /* the state may be in another app's memory: read it as the kernel */
    uint32_t cr3 = vmm_current();
    if (cr3 != g_kernel_pd_phys)
        vmm_load_kernel();
    window *w = *(window **)a->state;
    vmm_switch(cr3);
    return w;
}

app_instance_t *os_launch_app(app_descriptor *desc) {
    if (!desc) {
        ERR_WARN_REPORT(ERR_NULL_PTR, "os_launch_app");
        return NULL;
    }

    if (desc->single_instance) {
        for (int i = 0; i < MAX_RUNNING_APPS; i++) {
            app_instance_t *a = &running_apps[i];
            if (!a->running || a->wants_quit || a->desc != desc)
                continue;
            window *w = os_instance_window(a);
            if (w && w->visible)
                wm_focus(w);
            return a;
        }
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
    app->suspended = false;

    if (desc->state_size > 0) {
        app->state = desc->state_alloc
                         ? desc->state_alloc(desc, desc->state_size)
                         : kmalloc(desc->state_size);
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
    for (int i = 0; i < MAX_RUNNING_APPS; i++)
        if (running_apps[i].running && running_apps[i].desc == desc)
            running_apps[i].wants_quit = true;
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
        if (installed_apps[i]->name &&
            strcmp(installed_apps[i]->name, name) == 0)
            return installed_apps[i];
    return NULL;
}

void os_request_exit(void) { gui_should_exit = true; }

bool window_is_focused(window *win) {
    return win && (win == wm_focused_window());
}

bool os_close_own_instance(window *win) {
    if (!win)
        return false;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->wants_quit)
            continue;
        if (os_instance_window(a) != win)
            continue;
        {
            os_quit_app(a);
            return true;
        }
    }
    return false;
}

/* Run one on_frame for every live app (scheduler drives the frame rate). */
int g_os_stopped_app = -1;

app_instance_t *os_instance_of_window(window *w) {
    while (w && w->modal_owner) /* a dialog belongs to its owner's app */
        w = w->modal_owner;
    if (!w)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (a->running && os_instance_window(a) == w)
            return a;
    }
    return NULL;
}

static const char *fault_text(const appfault_t *f) {
    switch (f->vector) {
    case APPFAULT_KILLED:
        return "it was ended (not responding)";
    case 0:
        return "divide error";
    case 6:
        return "invalid opcode";
    case 7:
        return "no maths coprocessor";
    case 13:
        return "general protection fault";
    case 14:
        return f->cr2 < 0x1000 ? "NULL pointer" : "page fault";
    default:
        return "CPU exception";
    }
}

void os_guard_call(app_instance_t *culprit, void (*fn)(void *, void *), void *a,
                   void *b, const char *what) {
    if (culprit && culprit->suspended)
        return; /* not responding: leave it alone */
    appfault_t f;
    if (appguard_call2(fn, a, b, &f))
        return;
    vmm_load_kernel();
    if (f.vector == APPFAULT_STOPPED) {
        if (culprit) {
            culprit->suspended = true;
            g_os_stopped_app = (int)(culprit - running_apps);
        }
        return; /* the watchdog opens Monitor */
    }
    char msg[160];
    const char *name = culprit && culprit->desc && culprit->desc->name
                           ? culprit->desc->name
                           : "The system";
    if (f.vector == APPFAULT_KILLED)
        snprintf(msg, sizeof(msg), "%s was not responding\nand has been ended.",
                 name);
    else
        snprintf(msg, sizeof(msg), "%s stopped in %s:\n%s (EIP %08X)", name,
                 what, fault_text(&f), f.eip);
    if (culprit) {
        culprit->suspended = true; /* don't run its destroy either */
        os_quit_app(culprit);
    }
    dlg_post_message(f.vector == APPFAULT_KILLED ? "App Ended" : "App Crashed",
                     msg);
}

static void call_frame(void *fn, void *state) { ((app_frame_fn)fn)(state); }

void os_tick_apps(void) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *app = &running_apps[i];
        if (app->running && !app->wants_quit && !app->suspended && app->desc &&
            app->desc->on_frame)
            os_guard_call(app, call_frame, (void *)app->desc->on_frame,
                          app->state, "on_frame");
    }
}

void os_reap_dead_apps(void) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *app = &running_apps[i];
        if (!app->running || !app->wants_quit)
            continue;

        window *own = os_instance_window(app);
        if (app->desc && app->desc->destroy && !app->suspended)
            app->desc->destroy(app->state);
        if (own && wm_is_registered(own)) /* app didn't clean up: we do */
            wm_unregister(own);

        if (app->state) {
            if (app->desc && app->desc->state_free)
                app->desc->state_free(app->desc, app->state);
            else
                kfree(app->state);
            app->state = NULL;
        }

        if (app->task_slot >= 1)
            scheduler_kill_task(app->task_slot);

        app->running = false;
        app->suspended = false;
        if (g_os_stopped_app == i)
            g_os_stopped_app = -1;
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

    appload_gc();
    scheduler_reap();
}
