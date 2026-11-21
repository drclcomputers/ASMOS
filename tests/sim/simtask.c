/* Sim: a real cooperative scheduler (os/task_switch.asm, CR3 never set),
 * a scripted keyboard for the text shell, and a fake NIC that answers
 * ARP and ICMP echo. */
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "drivers/keyboard.h"
#include "drivers/ne2000.h"
#include "os/scheduler.h"
void wr(int fd, const char *s);
void sim_shot(const char *name);

task_t tasks[MAX_TASKS];
int current_task, task_count = 1;
extern void task_switch(uint32_t *old_esp, uint32_t new_esp, uint32_t cr3);
static uint8_t s_stacks[8][TASK_STACK_SIZE] __attribute__((aligned(16)));
static bool s_stack_used[8];

void scheduler_init(void) {
    memset(tasks, 0, sizeof(tasks));
    tasks[0].alive = true;
    current_task = 0;
    task_count = 1;
}

static void tramp(void) {
    task_t *t = &tasks[current_task];
    t->entry(t->arg);
    scheduler_exit_current();
}

int scheduler_add_task(void (*e)(void *), void *a, vmm_ctx_t *v) {
    (void)v;
    if (!tasks[0].alive)
        scheduler_init();
    int slot = -1, st = -1;
    for (int i = 1; i < MAX_TASKS && slot < 0; i++)
        if (!tasks[i].alive && !tasks[i].stack_base)
            slot = i;
    for (int i = 0; i < 8 && st < 0; i++)
        if (!s_stack_used[i])
            st = i;
    if (slot < 0 || st < 0)
        return -1;
    s_stack_used[st] = true;
    uint32_t *sp = (uint32_t *)(s_stacks[st] + TASK_STACK_SIZE);
    *--sp = 0;                       /* tramp's return address */
    *--sp = (uint32_t)(uintptr_t)tramp; /* ret */
    *--sp = 0x202;                   /* eflags */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; /* ebp ebx esi edi */
    tasks[slot] = (task_t){.esp = (uint32_t)(uintptr_t)sp,
                           .stack_base = s_stacks[st], .alive = true,
                           .entry = e, .arg = a};
    task_count++;
    return slot;
}

void task_yield(void) {
    int old = current_task, next = old;
    for (int k = 0; k < MAX_TASKS; k++) {
        next = (next + 1) % MAX_TASKS;
        if (tasks[next].alive)
            break;
    }
    if (next == old)
        return;
    current_task = next;
    task_switch(&tasks[old].esp, tasks[next].esp, 0);
}

void scheduler_exit_current(void) {
    tasks[current_task].alive = false;
    task_count--;
    for (;;)
        task_yield();
}
void scheduler_kill_task(int s) {
    if (s > 0 && tasks[s].alive) { tasks[s].alive = false; task_count--; }
}
void scheduler_reap(void) {
    for (int i = 1; i < MAX_TASKS; i++)
        if (!tasks[i].alive && tasks[i].stack_base && i != current_task) {
            for (int k = 0; k < 8; k++)
                if (tasks[i].stack_base == s_stacks[k])
                    s_stack_used[k] = false;
            tasks[i].stack_base = NULL;
        }
}

/* One scripted key per 40 calls: text, Enter/Backspace, TERM_KEY_* arrows,
 * '\x01'<c> screenshot, '\x02' wait, '\x03' end. */
extern volatile uint32_t pit_ticks, pit_seconds;
const char *g_sim_script;
static int s_calls;
void sim_cli_done(void);
void ps2_update(void) {
    pit_ticks += 1; /* every pass of the shell loop is a millisecond */
    pit_seconds = pit_ticks / 1000;
    kb.key_pressed = false;
    if (!g_sim_script || ++s_calls < 40)
        return;
    s_calls = 0;
    unsigned char c = (unsigned char)*g_sim_script;
    if (!c)
        return;
    g_sim_script++;
    if (c == 1) {
        char n[8] = "cli0";
        n[3] = *g_sim_script++;
        sim_shot(n);
        return;
    }
    if (c == 2) { s_calls = -2000; return; }
    if (c == 3) { sim_cli_done(); return; }
    kb.last_char = 0;
    kb.last_scancode = 0;
    kb.ctrl_c = c == 4;
    if (c == '\n') kb.last_scancode = ENTER;
    else if (c == '\b') kb.last_scancode = BACKSPACE;
    else if (c == 0x80) kb.last_scancode = UP_ARROW;
    else if (c == 0x81) kb.last_scancode = DOWN_ARROW;
    else if (c == 0x86) kb.last_scancode = ESC;
    else kb.last_char = (char)c;
    kb.key_pressed = true;
}

// fake NIC
static ne2000_rx_cb s_rx;
static uint8_t s_q[4][1600];
static uint16_t s_qlen[4];
static int s_qn;
int g_sim_pings;
void ne2000_set_rx_callback(ne2000_rx_cb cb) { s_rx = cb; }
void ne2000_poll(void) {
    pit_ticks += 1; /* time passes while code waits on the network */
    for (int i = 0; i < s_qn; i++)
        if (s_rx)
            s_rx(s_q[i], s_qlen[i]);
    s_qn = 0;
}
static uint16_t csum(const uint8_t *p, int n) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2)
        s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1)
        s += (uint32_t)p[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
bool ne2000_send(const uint8_t *d, uint16_t len) {
    static const uint8_t peer[6] = {2, 0, 0, 0, 0, 1};
    if (s_qn >= 4 || len > 1600)
        return true;
    uint8_t *r = s_q[s_qn];
    memcpy(r, d, len);
    memcpy(r, d + 6, 6);  /* to: the sender */
    memcpy(r + 6, peer, 6);
    if (d[12] == 0x08 && d[13] == 0x06 && d[21] == 1) { /* ARP request */
        r[21] = 2;
        memcpy(r + 22, peer, 6);
        memcpy(r + 28, d + 38, 4); /* sender IP = the asked IP */
        memcpy(r + 32, d + 22, 10); /* target = the asker */
        s_qlen[s_qn++] = len;
    } else if (d[12] == 0x08 && d[13] == 0x00 && d[23] == 1 && d[34] == 8) {
        memcpy(r + 26, d + 30, 4); /* swap IPs */
        memcpy(r + 30, d + 26, 4);
        r[34] = 0; /* echo reply */
        r[36] = r[37] = 0;
        int il = (d[16] << 8 | d[17]) - 20;
        uint16_t c = csum(r + 34, il);
        r[36] = (uint8_t)(c >> 8), r[37] = (uint8_t)c;
        r[24] = r[25] = 0;
        c = csum(r + 14, 20);
        r[24] = (uint8_t)(c >> 8), r[25] = (uint8_t)c;
        g_sim_pings++;
        s_qlen[s_qn++] = len;
    }
    return true;
}
void task_trampoline_c(int slot) { (void)slot; }
