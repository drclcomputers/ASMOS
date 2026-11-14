#include "os/appguard.h"
#include "lib/string.h"
#include "lib/vmm.h"
#include "os/error.h"
#include "os/os.h"
#include "os/scheduler.h"
#include "ui/modal.h"

typedef struct {
    uint32_t ebx, esi, edi, ebp, esp, eip;
} gjmp_t;

int guard_setjmp(gjmp_t *j) __attribute__((returns_twice));
void guard_longjmp(gjmp_t *j, int v) __attribute__((noreturn));

__asm__(
    ".text\n"
    ".globl guard_setjmp\n"
    "guard_setjmp:\n"
    "  movl 4(%esp), %eax\n"
    "  movl %ebx, 0(%eax)\n"
    "  movl %esi, 4(%eax)\n"
    "  movl %edi, 8(%eax)\n"
    "  movl %ebp, 12(%eax)\n"
    "  leal 4(%esp), %ecx\n"
    "  movl %ecx, 16(%eax)\n"
    "  movl (%esp), %ecx\n"
    "  movl %ecx, 20(%eax)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    ".globl guard_longjmp\n"
    "guard_longjmp:\n"
    "  movl 4(%esp), %edx\n"
    "  movl 8(%esp), %eax\n"
    "  movl 0(%edx), %ebx\n"
    "  movl 4(%edx), %esi\n"
    "  movl 8(%edx), %edi\n"
    "  movl 12(%edx), %ebp\n"
    "  movl 16(%edx), %esp\n"
    "  sti\n"
    "  jmp *20(%edx)\n"

    /* exception stubs: stack on entry = [err][eip][cs][eflags] (or no err) */
    ".macro EXC_NOERR n\n"
    ".globl isr_exc\\n\n"
    "isr_exc\\n:\n"
    "  pushl $0\n"
    "  pushl $\\n\n"
    "  jmp exc_common\n"
    ".endm\n"
    ".macro EXC_ERR n\n"
    ".globl isr_exc\\n\n"
    "isr_exc\\n:\n"
    "  pushl $\\n\n"
    "  jmp exc_common\n"
    ".endm\n"
    "EXC_NOERR 0\n"
    "EXC_NOERR 1\n"
    "EXC_NOERR 2\n"
    "EXC_NOERR 3\n"
    "EXC_NOERR 4\n"
    "EXC_NOERR 5\n"
    "EXC_NOERR 6\n"
    "EXC_NOERR 7\n"
    "EXC_ERR 8\n"
    "EXC_NOERR 9\n"
    "EXC_ERR 10\n"
    "EXC_ERR 11\n"
    "EXC_ERR 12\n"
    "EXC_ERR 13\n"
    "EXC_ERR 14\n"
    "EXC_NOERR 15\n"
    "EXC_NOERR 16\n"
    "EXC_ERR 17\n"
    "EXC_NOERR 18\n"
    "EXC_NOERR 19\n"
    "EXC_NOERR 20\n"
    "EXC_NOERR 21\n"
    "EXC_NOERR 22\n"
    "EXC_NOERR 23\n"
    "EXC_NOERR 24\n"
    "EXC_NOERR 25\n"
    "EXC_NOERR 26\n"
    "EXC_NOERR 27\n"
    "EXC_NOERR 28\n"
    "EXC_NOERR 29\n"
    "EXC_NOERR 30\n"
    "EXC_NOERR 31\n"
    "exc_common:\n"
    "  pusha\n"
    "  movl %cr2, %eax\n"
    "  pushl %eax\n"
    "  pushl 44(%esp)\n" /* eip */
    "  pushl 44(%esp)\n" /* err */
    "  pushl 44(%esp)\n" /* vec */
    "  call exc_handler_c\n"
    "  cli\n"
    "1: hlt\n"
    "  jmp 1b\n"
    ".section .rodata\n"
    ".globl exc_stub_table\n"
    "exc_stub_table:\n"
    "  .long isr_exc0\n"
    "  .long isr_exc1\n"
    "  .long isr_exc2\n"
    "  .long isr_exc3\n"
    "  .long isr_exc4\n"
    "  .long isr_exc5\n"
    "  .long isr_exc6\n"
    "  .long isr_exc7\n"
    "  .long isr_exc8\n"
    "  .long isr_exc9\n"
    "  .long isr_exc10\n"
    "  .long isr_exc11\n"
    "  .long isr_exc12\n"
    "  .long isr_exc13\n"
    "  .long isr_exc14\n"
    "  .long isr_exc15\n"
    "  .long isr_exc16\n"
    "  .long isr_exc17\n"
    "  .long isr_exc18\n"
    "  .long isr_exc19\n"
    "  .long isr_exc20\n"
    "  .long isr_exc21\n"
    "  .long isr_exc22\n"
    "  .long isr_exc23\n"
    "  .long isr_exc24\n"
    "  .long isr_exc25\n"
    "  .long isr_exc26\n"
    "  .long isr_exc27\n"
    "  .long isr_exc28\n"
    "  .long isr_exc29\n"
    "  .long isr_exc30\n"
    "  .long isr_exc31\n"
    ".text\n");

/* One stub per CPU exception 0..31. Before, only 0/6/13/14 had handlers;
 * the rest hit isr_spurious, which sent a PIC EOI and IRET'd with the
 * error code still on the stack (garbage EIP → triple fault / reboot). */
extern const uint32_t exc_stub_table[32];

/* ── Unguarded fault fallback ───────────────────────────────────────────────
 * Reached for CPU exceptions outside appguard_call2. A background task
 * (shell `run`, gopher, ...) is killed; the kernel task panics visibly. */
#define PF_PRESENT (1u << 0) /* 0=not-present, 1=protection violation */
#define PF_WRITE (1u << 1)
#define PF_RSVD (1u << 3)
#define PF_IFETCH (1u << 4)

static const char *exc_name(uint32_t v) {
    switch (v) {
    case 0:
        return "divide error";
    case 6:
        return "invalid opcode";
    case 8:
        return "double fault";
    case 12:
        return "stack fault";
    case 13:
        return "general protection fault";
    case 14:
        return "page fault";
    default:
        return "CPU exception";
    }
}

static void fault_kill_task(uint32_t vec, uint32_t eip, uint32_t err,
                            uint32_t fault_addr) {
    app_instance_t *victim = NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (running_apps[i].running &&
            running_apps[i].task_slot == current_task) {
            victim = &running_apps[i];
            break;
        }
    }
    const char *app_name = (victim && victim->desc && victim->desc->name)
                               ? victim->desc->name
                               : "Unknown App";

    const char *kind = exc_name(vec);
    if (vec != 14)
        ;
    else if (err & PF_RSVD)
        kind = "reserved-bit violation";
    else if (err & PF_IFETCH)
        kind = "instruction fetch violation";
    else if (err & PF_WRITE)
        kind = (err & PF_PRESENT) ? "write protection fault"
                                  : "write to unmapped page";
    else
        kind = (err & PF_PRESENT) ? "read protection fault"
                                  : "read from unmapped page";

    char msg[128];
    if (vec == 14)
        snprintf(msg, sizeof(msg), "%.60s: %s\nat 0x%08X (EIP 0x%08X)",
                 app_name, kind, fault_addr, eip);
    else
        snprintf(msg, sizeof(msg), "%.60s: %s #%u\n(EIP 0x%08X)", app_name,
                 kind, vec, eip);

    vmm_load_kernel();
    modal_show(MODAL_ERROR, "Task Fault — Terminated", msg, NULL, NULL);
    if (victim)
        os_quit_app(victim);
    /* Never returns. (Old code yielded then hlt-looped with IF=0: once the
     * scheduler came back to this task the whole OS froze.) */
    scheduler_exit_current();
}

static gjmp_t *s_jmp = NULL;
static appfault_t s_fault;

void exc_handler_c(uint32_t vec, uint32_t err, uint32_t eip, uint32_t cr2) {
    if (s_jmp) {
        gjmp_t *j = s_jmp;
        s_jmp = NULL;
        s_fault.vector = vec;
        s_fault.error = err;
        s_fault.eip = eip;
        s_fault.cr2 = cr2;
        guard_longjmp(j, 1);
    }
    if (current_task != 0)
        fault_kill_task(vec, eip, err, cr2);

    /* Kernel task, unguarded: show what happened instead of a silent hang. */
    char msg[80];
    snprintf(msg, sizeof(msg), "%s #%u at EIP 0x%08X, err 0x%X, CR2 0x%08X",
             exc_name(vec), vec, eip, err, cr2);
    error_set_gui_mode(false);
    error_report(ERR_FATAL, ERR_CPU_EXCEPTION, msg); /* halts */
    __asm__ volatile("cli");
    for (;;)
        __asm__ volatile("hlt");
}

bool appguard_call2(void (*fn)(void *, void *), void *a, void *b,
                    appfault_t *out) {
    gjmp_t jb;
    gjmp_t *prev = s_jmp;
    volatile bool ok = true;

    if (guard_setjmp(&jb) == 0) {
        s_jmp = &jb;
        fn(a, b);
        s_jmp = prev;
    } else {
        s_jmp = prev; /* fault: handler already cleared s_jmp */
        ok = false;
        if (out)
            *out = s_fault;
    }
    return ok;
}

void appguard_install_gates(void (*set_gate)(uint8_t, uint32_t)) {
    for (int v = 0; v < 32; v++)
        if (v != 2) /* NMI is an interrupt, not a fault: keep the default */
            set_gate((uint8_t)v, exc_stub_table[v]);
}
