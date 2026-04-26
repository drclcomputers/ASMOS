[bits 32]
global _start
extern kmain
extern _bss_start
extern _bss_end
extern _kstack_top

_start:
    mov ebp, _kstack_top
    mov esp, ebp

    xor eax, eax
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi
    rep stosb

    call kmain

    jmp $
