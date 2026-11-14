#include "os/appguard.h"
#include "os/scheduler.h"

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
    "EXC_NOERR 6\n"
    "EXC_ERR 13\n"
    "EXC_ERR 14\n"
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
    "  jmp 1b\n");

extern void isr_exc0(void);
extern void isr_exc6(void);
extern void isr_exc13(void);
extern void isr_exc14(void);
extern void pf_handler_c(uint32_t eip, uint32_t err, uint32_t addr);

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
    if (vec == 14)
        pf_handler_c(eip, err, cr2); /* task-level handler; halts if task 0 */
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
    set_gate(0, (uint32_t)isr_exc0);
    set_gate(6, (uint32_t)isr_exc6);
    set_gate(13, (uint32_t)isr_exc13);
    set_gate(14, (uint32_t)isr_exc14);
}
