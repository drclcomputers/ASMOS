#include "os/os.h"
#include "os/error.h"
#include "lib/primitive_graphics.h"
#include "lib/mem.h"
#include "lib/alloc.h"
#include "lib/string.h"
#include "ui/ui.h"
#include "ui/modal.h"
#include "io/ps2.h"
#include "config/config.h"

menubar g_menubar;

app_descriptor *installed_apps[MAX_INSTALLED_APPS];
int installed_app_count = 0;

app_instance_t  running_apps[MAX_RUNNING_APPS];
int running_app_count = 0;

static bool gui_should_exit = false;

bool g_menubar_click_consumed = false;

void os_install_app(app_descriptor *desc) {
    if (!desc) {
        ERR_WARN_REPORT(ERR_NULL_PTR, "os_install_app: desc");
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
        ERR_WARN_REPORT(ERR_NULL_PTR, "os_launch_app: desc");
        return NULL;
    }
    if (running_app_count >= MAX_RUNNING_APPS) {
        ERR_WARN_REPORT(ERR_APP_MAX_RUNNING, "os_launch_app");
        return NULL;
    }

    app_instance_t *inst = NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (!running_apps[i].running) { inst = &running_apps[i]; break; }
    }
    if (!inst) {
        ERR_WARN_REPORT(ERR_APP_MAX_RUNNING, "os_launch_app: no free slot");
        return NULL;
    }

    void *state = NULL;
    if (desc->state_size > 0) {
        state = kmalloc(desc->state_size);
        if (!state) {
            ERR_WARN_REPORT(ERR_APP_ALLOC, desc->name ? desc->name : "?");
            return NULL;
        }
        memset(state, 0, desc->state_size);
    }

    inst->desc    = desc;
    inst->state   = state;
    inst->running = true;
    running_app_count++;

    if (desc->init) desc->init(state);

    return inst;
}

void os_quit_app(app_instance_t *inst) {
    if (!inst || !inst->running) return;

    if (inst->desc && inst->desc->destroy)
        inst->desc->destroy(inst->state);

    if (inst->state) {
        kfree(inst->state);
        inst->state = NULL;
    }

    inst->desc    = NULL;
    inst->running = false;
    running_app_count--;

    for (int i = win_count - 1; i >= 0; i--) {
        if (win_stack[i]->visible && !win_stack[i]->minimized) {
            wm_focus(win_stack[i]);
            break;
        }
    }
}

void os_quit_app_by_desc(app_descriptor *desc) {
    if (!desc) return;
    app_instance_t *inst = os_find_instance(desc);
    if (inst) os_quit_app(inst);
}

app_instance_t *os_find_instance(app_descriptor *desc) {
    if (!desc) return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (running_apps[i].running && running_apps[i].desc == desc)
            return &running_apps[i];
    }
    return NULL;
}

int os_find_instances(app_descriptor *desc, app_instance_t **out, int max) {
    if (!desc || !out) return 0;
    int found = 0;
    for (int i = 0; i < MAX_RUNNING_APPS && found < max; i++)
        if (running_apps[i].running && running_apps[i].desc == desc)
            out[found++] = &running_apps[i];
    return found;
}

app_descriptor *os_find_app(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < installed_app_count; i++) {
        if (strcmp(installed_apps[i]->name, name) == 0)
            return installed_apps[i];
    }
    return NULL;
}

void os_run(void) {
    gui_should_exit = false;

    while (!gui_should_exit) {
        g_menubar_click_consumed = false;

        ps2_update();

        wm_sync_menubar(&g_menubar);
        menubar_layout(&g_menubar);

        if (!modal_active()) {
            menubar_update(&g_menubar);
        }

        for (int i = 0; i < MAX_RUNNING_APPS; i++) {
            if (running_apps[i].running && running_apps[i].desc &&
                running_apps[i].desc->on_frame)
                running_apps[i].desc->on_frame(running_apps[i].state);
        }

        desktop_on_frame();

        wm_sync_menubar(&g_menubar);
        menubar_layout(&g_menubar);

        if (!modal_active()) {
        	wm_update_all();
        }

        //mouse.left_clicked = false;

        wm_draw_all();
        menubar_draw(&g_menubar);

        modal_update();
        modal_draw();

        draw_cursor(mouse.x, mouse.y);
        blit();
    }

    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (running_apps[i].running)
            os_quit_app(&running_apps[i]);
    }
}

void os_request_exit(void) {
    gui_should_exit = true;
}
