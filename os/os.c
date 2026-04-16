#include "os/os.h"
#include "os/error.h"
#include "os/app_registry.h"
#include "os/scheduler.h"

#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "lib/device.h"

#include "ui/ui.h"
#include "ui/modal.h"

#include "io/ps2.h"
#include "config/config.h"

menubar g_menubar;
bool    g_menubar_click_consumed = false;
bool    gui_should_exit          = false;

app_descriptor *installed_apps[MAX_INSTALLED_APPS];
int             installed_app_count = 0;

app_instance_t  running_apps[MAX_RUNNING_APPS];
int             running_app_count = 0;

/*
 * This is the entry function stored in each app's task_t.
 * task_trampoline_c calls it once per scheduler turn, then calls
 * task_yield() to hand control back to the kernel task.
 */
static void app_task_entry(void *state)
{
    app_instance_t *inst = (app_instance_t *)state;
    if (!inst || !inst->running) return;
    if (inst->desc && inst->desc->on_frame)
        inst->desc->on_frame(inst->state);
}

void os_install_app(app_descriptor *desc)
{
    if (!desc) { ERR_WARN_REPORT(ERR_NULL_PTR, "os_install_app"); return; }
    if (installed_app_count >= MAX_INSTALLED_APPS) {
        ERR_WARN_REPORT(ERR_APP_MAX_RUNNING, "os_install_app: registry full"); return;
    }
    installed_apps[installed_app_count++] = desc;
}

app_instance_t *os_launch_app(app_descriptor *desc)
{
    if (!desc) { ERR_WARN_REPORT(ERR_NULL_PTR, "os_launch_app"); return NULL; }
    if (running_app_count >= MAX_RUNNING_APPS) {
        ERR_WARN_REPORT(ERR_APP_MAX_RUNNING, "os_launch_app"); return NULL;
    }

    app_instance_t *inst = NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (!running_apps[i].running) { inst = &running_apps[i]; break; }
    }
    if (!inst) {
        ERR_WARN_REPORT(ERR_APP_MAX_RUNNING, "os_launch_app: no slot"); return NULL;
    }

    void *state = NULL;
    if (desc->state_size > 0) {
        state = kmalloc(desc->state_size);
        if (!state) {
            ERR_WARN_REPORT(ERR_APP_ALLOC, desc->name ? desc->name : "?"); return NULL;
        }
        memset(state, 0, desc->state_size);
    }

    inst->desc      = desc;
    inst->state     = state;
    inst->running   = true;
    inst->task_slot = -1;
    running_app_count++;

    if (desc->init) desc->init(state);

    int slot = scheduler_add_task(app_task_entry, (void *)inst);
    if (slot < 0)
        ERR_WARN_REPORT(ERR_APP_ALLOC, "scheduler_add_task: out of slots");
    inst->task_slot = slot;

    return inst;
}

void os_quit_app(app_instance_t *inst)
{
    if (!inst || !inst->running) return;

    /* Kill the scheduler task so it won't be entered again.
       If we are currently executing ON that task's stack (i.e. the app
       called os_quit_app on itself), scheduler_remove_task just marks
       alive=false; task_trampoline_c's loop exits on the next check and
       parks in task_yield() until the kernel cleans up. */
    if (inst->task_slot >= 0) {
        scheduler_remove_task(inst->task_slot);
        inst->task_slot = -1;
    }

    if (inst->desc && inst->desc->destroy)
        inst->desc->destroy(inst->state);

    if (inst->state) { kfree(inst->state); inst->state = NULL; }

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

void os_quit_app_by_desc(app_descriptor *desc)
{
    if (!desc) return;
    app_instance_t *inst = os_find_instance(desc);
    if (inst) os_quit_app(inst);
}

app_instance_t *os_find_instance(app_descriptor *desc)
{
    if (!desc) return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++)
        if (running_apps[i].running && running_apps[i].desc == desc)
            return &running_apps[i];
    return NULL;
}

int os_find_instances(app_descriptor *desc, app_instance_t **out, int max)
{
    if (!desc || !out) return 0;
    int found = 0;
    for (int i = 0; i < MAX_RUNNING_APPS && found < max; i++)
        if (running_apps[i].running && running_apps[i].desc == desc)
            out[found++] = &running_apps[i];
    return found;
}

app_descriptor *os_find_app(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < installed_app_count; i++)
        if (strcmp(installed_apps[i]->name, name) == 0)
            return installed_apps[i];
    return NULL;
}

void os_request_exit(void) { gui_should_exit = true; }

/*
 * Full-stack-switching bootstrap.
 *
 * Execution flow:
 *
 *  kmain()
 *    └─ os_run()
 *         1. scheduler_init()   — builds slot 0 (kernel task) on s_kernel_stack
 *         2. saves own esp into scheduler_exit_esp
 *         3. task_switch(&dummy, tasks[0].saved_esp)
 *              └─ jumps to task_trampoline → task_trampoline_c(0)
 *                    ├─ [kernel frame loop] scheduler_kernel_task() → task_yield()
 *                    │       └─ switches to each app task in turn
 *                    │             app task: on_frame() → task_yield() → back to kernel
 *                    └─ when gui_should_exit:
 *                          task_switch(&tasks[0].saved_esp, scheduler_exit_esp)
 *                              └─ returns here (step 4 below)
 *         4. cleanup: destroy all running apps
 *         5. return to kmain
 */

extern void task_switch(uint32_t *old_esp_ptr, uint32_t new_esp);

void os_run(void)
{
    gui_should_exit = false;

    scheduler_init();   /* builds kernel task in slot 0 */

    task_switch(&scheduler_exit_esp, tasks[0].saved_esp);

    for (int i = 0; i < MAX_RUNNING_APPS; i++)
        if (running_apps[i].running)
            os_quit_app(&running_apps[i]);

    for (int i = 1; i < task_count; i++) {
        if (tasks[i].stack_base) {
            kfree(tasks[i].stack_base);
            tasks[i].stack_base = NULL;
        }
        tasks[i].alive = false;
    }
}
