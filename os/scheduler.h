#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "lib/core.h"

#define TASK_STACK_SIZE (16 * 1024)

typedef struct task {
    uint32_t  saved_esp;       // asm touches offset 0
    uint8_t  *stack_base;
    bool      alive;
    bool      started;
    void     *app_state;
    void    (*entry)(void *);  // app descriptor's on_frame wrapper
} task_t;

#define MAX_TASKS 64

extern task_t  tasks[MAX_TASKS];
extern int     task_count;
extern int     current_task;

extern uint32_t scheduler_exit_esp;

void scheduler_init(void);

int  scheduler_add_task(void (*entry)(void *), void *state);
void scheduler_remove_task(int slot);

void task_yield(void);

void scheduler_kernel_task(void *unused);

void task_trampoline_c(int slot);

#endif
