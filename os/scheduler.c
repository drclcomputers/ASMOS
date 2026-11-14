#include "os/scheduler.h"
#include "os/error.h"
#include "os/os.h"

#include "lib/device.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

#include "config/config.h"
#include "drivers/opl2.h"
#include "io/ps2.h"
#include "network/net.h"
#include "ui/ui.h"

task_t tasks[MAX_TASKS];
int current_task = 0;
int task_count = 0;

#define MAX_PENDING_FREES 64
static void *s_pending_free[MAX_PENDING_FREES];
static int s_pending_free_count = 0;

extern void task_switch(uint32_t *old_esp, uint32_t new_esp, uint32_t new_cr3);

// Init
void scheduler_init(void) {
    memset(tasks, 0, sizeof(tasks));

    tasks[0].alive = true;
    tasks[0].stack_base = NULL;
    tasks[0].vmm = NULL;
    current_task = 0;
    task_count = 1;
}

// Trampoline
void task_trampoline_c(int slot) {
    if (slot == 0) {
        scheduler_kernel_task();
    } else {
        task_t *t = &tasks[slot];
        if (t->entry)
            t->entry(t->arg);
    }
    scheduler_exit_current();
    while (1)
        task_yield();
}

// Deferred stack free
static void enqueue_stack_free(void *stack) {
    if (!stack)
        return;
    if (s_pending_free_count < MAX_PENDING_FREES)
        s_pending_free[s_pending_free_count++] = stack;
}

void scheduler_reap(void) {
    for (int i = 0; i < s_pending_free_count; i++) {
        kfree(s_pending_free[i]);
        s_pending_free[i] = NULL;
    }
    s_pending_free_count = 0;
}

// Add task
int scheduler_add_task(void (*entry)(void *), void *arg, vmm_ctx_t *vmm) {
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (!tasks[i].alive) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack)
        return -1;

    tasks[slot].entry = entry;
    tasks[slot].arg = arg;
    tasks[slot].stack_base = stack;
    tasks[slot].vmm = vmm;

    *(uint32_t *)stack = STACK_CANARY;

    uint32_t *sp = (uint32_t *)(stack + TASK_STACK_SIZE);
    extern void task_trampoline(void);

    *--sp = (uint32_t)slot;
    *--sp = 0;
    *--sp = (uint32_t)task_trampoline;
    *--sp = 0x202; // eflags: IF=1
    *--sp = 0;     // ebp
    *--sp = 0;     // ebx
    *--sp = 0;     // esi
    *--sp = 0;     // edi

    tasks[slot].esp = (uint32_t)sp;
    tasks[slot].alive = true;
    task_count++;
    return slot;
}

// Yield / context switch
void task_yield(void) {
    int old = current_task;

    if (old > 0 && tasks[old].stack_base &&
        *(uint32_t *)tasks[old].stack_base != STACK_CANARY) {
        error_report(ERR_FATAL, ERR_TASK_STACK_OVERFLOW, "task stack overflow");
    }

    int next = old;
    for (int tries = 0; tries < MAX_TASKS; tries++) {
        next = (next + 1) % MAX_TASKS;
        if (tasks[next].alive)
            break;
    }
    if (next == old)
        return;

    uint32_t new_cr3 = 0;
    if (tasks[next].vmm && tasks[next].vmm->pd_phys)
        new_cr3 = tasks[next].vmm->pd_phys;
    else
        new_cr3 = g_kernel_pd_phys;

    uint32_t cur_cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    if (new_cr3 == cur_cr3)
        new_cr3 = 0;

    current_task = next;
    task_switch(&tasks[old].esp, tasks[next].esp, new_cr3);
}

// Kill / exit
void scheduler_kill_task(int slot) {
    if (slot < 1 || slot >= MAX_TASKS)
        return;
    if (!tasks[slot].alive)
        return;

    tasks[slot].alive = false;
    task_count--;

    if (tasks[slot].vmm) {
        vmm_destroy_context(tasks[slot].vmm);
        tasks[slot].vmm = NULL;
    }

    enqueue_stack_free(tasks[slot].stack_base);
    tasks[slot].stack_base = NULL;
}

void scheduler_exit_current(void) {
    int slot = current_task;
    if (slot < 1 || slot >= MAX_TASKS) {
        while (1)
            task_yield();
    }

    if (tasks[slot].vmm) {
        vmm_destroy_context(tasks[slot].vmm);
        tasks[slot].vmm = NULL;
    }

    enqueue_stack_free(tasks[slot].stack_base);
    tasks[slot].stack_base = NULL;
    tasks[slot].alive = false;
    task_count--;

    while (1)
        task_yield();
}

// Global shortcut handler
extern app_descriptor monitor_app;

static void handle_global_shortcuts(void) {
    /* Ctrl+Shift+Esc: open / bring Monitor to front */
    if (kb.key_pressed && kb.ctrl && kb.shift && kb.last_scancode == ESC) {
        kb.key_pressed = false;
        app_instance_t *m = os_launch_app(&monitor_app);
        window *w = os_instance_window(m);
        if (w) {
            w->minimized = false;
            w->visible = true;
            wm_focus(w);
        }
        return;
    }

    if (kb.key_pressed && kb.ctrl && kb.last_scancode == Q_KEY) {
        window *fw = wm_focused_window();
        if (fw && fw->on_close) {
            kb.key_pressed = false;
            fw->on_close(fw);
        }
    }
}

// Kernel main loop
void scheduler_kernel_task(void) {
    while (!gui_should_exit) {
        uint32_t frame_start = time_millis();

        g_menubar_click_consumed = false;
        ps2_update();
        net_poll();
        speaker_update();
        midi_player_update();

        handle_global_shortcuts();

        wm_sync_menubar(&g_menubar);
        menubar_layout(&g_menubar);

        if (!modal_active()) {
            menubar_update(&g_menubar);
            os_tick_apps_direct();
            wm_update_all();
        }

        os_reap_dead_apps();

        desktop_on_frame();
        wm_draw_all();
        menubar_draw(&g_menubar);

        modal_update();
        modal_draw();

        draw_cursor(mouse.x, mouse.y);

        blit();

        while ((time_millis() - frame_start) < FRAME_TIME_MS) {
            task_yield();
        }
    }
}
