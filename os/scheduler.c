#include "os/scheduler.h"
#include "os/os.h"
#include "os/error.h"

#include "lib/memory.h"
#include "lib/string.h"
#include "lib/graphics.h"
#include "lib/time.h"
#include "lib/device.h"

#include "ui/ui.h"
#include "io/ps2.h"
#include "config/config.h"

task_t   tasks[MAX_TASKS];
int      task_count    = 0;
int      current_task  = 0;

uint32_t scheduler_exit_esp = 0;

static uint8_t s_kernel_stack[TASK_STACK_SIZE];

extern void task_switch(uint32_t *old_esp_ptr, uint32_t new_esp);
extern void task_trampoline(void);

/* Lay out a brand-new task's initial stack so that when task_switch pops
 * the saved frame and executes ret, execution lands in task_trampoline
 * with `slot` already sitting in the correct cdecl argument position.
 *
 * Stack layout (high address → low address):
 *
 *   [stack_base + TASK_STACK_SIZE]   ← top (unused guard)
 *   [ 0 ]                            fake ret-addr for task_trampoline_c
 *   [ slot ]                         cdecl arg 1 for task_trampoline_c
 *   [ &task_trampoline ]             ret-addr that task_switch's ret jumps to
 *   [ 0 ]                            fake edi  ─┐
 *   [ 0 ]                            fake esi   │  popped by task_switch
 *   [ 0 ]                            fake ebx   │  before the ret
 *   [ 0 ]                            fake ebp   │
 *   [ 0x00000202 ]                   eflags IF=1─┘
 *   ← saved_esp points here
 *
 * task_switch restores: edi, esi, ebx, ebp, eflags  (5 pops)
 * then ret → &task_trampoline
 *
 * task_trampoline (asm):
 *   add esp, 4      ; discard its own address (the ret-addr we jumped to)
 *   jmp task_trampoline_c
 *
 * After that add, the stack looks like a normal cdecl call:
 *   [esp+0] = fake ret-addr (0)  ← task_trampoline_c's return address
 *   [esp+4] = slot               ← first argument
 */

static uint32_t build_initial_stack(uint8_t *stack_base, int slot)
{
    uint32_t *sp = (uint32_t *)(stack_base + TASK_STACK_SIZE);

    *--sp = (uint32_t)slot;
    *--sp = 0;
    *--sp = (uint32_t)task_trampoline;
    *--sp = 0x00000202;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    return (uint32_t)sp;
}

void task_trampoline_c(int slot)
{
    task_t *t = &tasks[slot];

    if (slot == 0) {
        extern bool gui_should_exit;

        while (!gui_should_exit) {
            if (t->entry) t->entry(t->app_state);
            task_yield();
        }

        t->alive = false;

        task_switch(&t->saved_esp, scheduler_exit_esp);

        for (;;) { __asm__ volatile ("hlt"); }

    } else {
        while (t->alive) {
            if (t->entry) t->entry(t->app_state);
            task_yield();
        }
        while (1) task_yield();
    }
}

int scheduler_add_task(void (*entry)(void *), void *state)
{
    /* Slot 0 is reserved for the kernel task */
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (!tasks[i].alive) { slot = i; break; }
    }
    if (slot < 0) return -1;

    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) return -1;
    memset(stack, 0, TASK_STACK_SIZE);

    task_t *t     = &tasks[slot];
    t->stack_base = stack;
    t->entry      = entry;
    t->app_state  = state;
    t->alive      = true;
    t->saved_esp  = build_initial_stack(stack, slot);

    if (slot >= task_count) task_count = slot + 1;

    return slot;
}

void scheduler_remove_task(int slot)
{
    if (slot < 1 || slot >= MAX_TASKS) return;
    task_t *t = &tasks[slot];
    t->alive  = false;
    if (t->stack_base) {
        kfree(t->stack_base);
        t->stack_base = NULL;
    }
}

void task_yield(void)
{
    int old  = current_task;
    int next = old;

    for (int i = 1; i <= task_count; i++) {
        int c = (old + i) % task_count;
        if (tasks[c].alive) { next = c; break; }
    }

    if (next == old) return;

    current_task = next;
    task_switch(&tasks[old].saved_esp, tasks[next].saved_esp);
}

/*
 * One frame of kernel work — called by task_trampoline_c for slot 0.
 * Input → logic → desktop → WM → draw → frame-rate cap.
 * After this returns, task_trampoline_c calls task_yield() so every
 * app task gets exactly one on_frame call per kernel frame.
 */
void scheduler_kernel_task(void *unused)
{
    (void)unused;
    extern menubar g_menubar;
    extern bool    g_menubar_click_consumed;

    uint32_t frame_start = time_millis();

    // 1. Core Kernel Logic (Runs once per 16ms)
    g_menubar_click_consumed = false;
    ps2_update();    // Only poll here! This ensures logic sees the state.
    speaker_update();

    wm_sync_menubar(&g_menubar);
    menubar_layout(&g_menubar);

    if (!modal_active())
        menubar_update(&g_menubar);

    desktop_on_frame();

    wm_sync_menubar(&g_menubar);
    menubar_layout(&g_menubar);

    if (!modal_active())
        wm_update_all();

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

void scheduler_init(void)
{
    memset(tasks, 0, sizeof(tasks));
    task_count    = 1;
    current_task  = 0;

    task_t *kt    = &tasks[0];
    kt->stack_base = s_kernel_stack;
    kt->entry      = scheduler_kernel_task;
    kt->app_state  = NULL;
    kt->alive      = true;
    kt->saved_esp  = build_initial_stack(s_kernel_stack, 0);
}
