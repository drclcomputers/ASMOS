[bits 32]
global _start
extern kmain

_start:
    mov ebp, 0x90000
    mov esp, ebp

    call kmain

    jmp $
