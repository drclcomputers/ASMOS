.globl _start
_start:
    mov (%esp), %eax        /* argc */
    lea 4(%esp), %ebx       /* argv */
    push %ebx
    push %eax
    call sim_main
    mov %eax, %ebx
    mov $1, %eax
    int $0x80
