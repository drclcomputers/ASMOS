#include "os/scheduler.h"
#include "os/error.h"
#include "os/os.h"

#include "lib/device.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

#include "config/config.h"
#include "io/ps2.h"
#include "ui/ui.h"

#include "drivers/opl2.h"

task_t tasks[MAX_TASKS];
int current_task = 0;
int task_count = 0;

#define MAX_PENDING_FREES 64
static void *s_pending_free[MAX_PENDING_FREES];
static int s_pending_free_count = 0;

extern void scheduler_kernel_task(void);

void scheduler_init(void) {
    memset(tasks, 0, sizeof(tasks));

    tasks[0].alive = true;
    tasks[0].stack_base = NULL;
    /* tasks[0] is written by the first task_switch out of the kernel task */
    current_task = 0;
    task_count = 1;
}

void task_trampoline_c(int slot) {
    if (slot == 0) {
        scheduler_kernel_task();
    } else {
        task_t *t = &tasks[slot];
        if (t->entry) {
            t->entry(t->arg);
        }
    }

    scheduler_exit_current();
    while (1) {
        task_yield();
    }
}

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

extern void task_switch(uint32_t *old_esp, uint32_t new_esp);

int scheduler_add_task(void (*entry)(void *), void *arg) {
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

    *(uint32_t *)stack = STACK_CANARY;

    uint32_t *sp = (uint32_t *)(stack + TASK_STACK_SIZE);
    extern void task_trampoline(void);

    *--sp = (uint32_t)slot;
    *--sp = 0;
    *--sp = (uint32_t)task_trampoline;

    *--sp = 0x202; // eflags (Highest address, popped last)
    *--sp = 0;     // ebp
    *--sp = 0;     // ebx
    *--sp = 0;     // esi
    *--sp = 0;     // edi

    tasks[slot].esp = (uint32_t)sp;
    tasks[slot].alive = true;
    task_count++;
    return slot;
}

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

    current_task = next;
    task_switch(&tasks[old].esp, tasks[next].esp);
}

void scheduler_kill_task(int slot) {
    if (slot < 1 || slot >= MAX_TASKS)
        return;
    if (!tasks[slot].alive)
        return;

    tasks[slot].alive = false;
    task_count--;

    enqueue_stack_free(tasks[slot].stack_base);
    tasks[slot].stack_base = NULL;
}

void scheduler_exit_current(void) {
    int slot = current_task;
    if (slot < 1 || slot >= MAX_TASKS) {
        while (1)
            task_yield();
    }

    enqueue_stack_free(tasks[slot].stack_base);
    tasks[slot].stack_base = NULL;
    tasks[slot].alive = false;
    task_count--;

    while (1)
        task_yield();
}

static void handle_global_shortcuts(void) {
    if (kb.key_pressed && kb.ctrl && kb.last_scancode == Q_KEY) {
        window *fw = wm_focused_window();
        if (fw && fw->on_close) {
            kb.key_pressed = false;
            fw->on_close(fw);
        }
    }
}

/*static void handle_global_shortcuts(void) {
    if (!kb.key_pressed)
        return;

    if (kb.ctrl) {
        uint8_t sc = kb.last_scancode;
        if (sc == Q_KEY) {
            window *fw = wm_focused_window();
            if (fw && fw->on_close) {
                kb.key_pressed = false;
                fw->on_close(fw);
            }
            return;
        }
        if (sc == C_KEY) { kb.ctrl_c = true; kb.key_pressed = false; return; }
        if (sc == X_KEY) { kb.ctrl_x = true; kb.key_pressed = false; return; }
        if (sc == V_KEY) { kb.ctrl_v = true; kb.key_pressed = false; return; }
        if (sc == A_KEY) { kb.ctrl_a = true; kb.key_pressed = false; return; }
    }
} */

void scheduler_kernel_task(void) {
    extern menubar g_menubar;

    while (!gui_should_exit) {
        uint32_t frame_start = time_millis();

        g_menubar_click_consumed = false;
        ps2_update();
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
