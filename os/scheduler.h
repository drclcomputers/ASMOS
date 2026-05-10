#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "lib/core.h"

#define MAX_TASKS 128
#define TASK_STACK_SIZE (16 * 1024)
#define STACK_CANARY 0xDEADBEEFU

typedef struct {
    uint32_t esp;
    uint8_t *stack_base;
    bool alive;
    void (*entry)(void *);
    void *arg;
} task_t;

extern task_t tasks[MAX_TASKS];
extern int current_task;
extern int task_count;

void scheduler_init(void);
int scheduler_add_task(void (*entry)(void *), void *arg);
void task_yield(void);
void scheduler_kill_task(int slot);
void scheduler_exit_current(void);
void scheduler_reap(void);
void scheduler_kernel_task(void);

#endif
