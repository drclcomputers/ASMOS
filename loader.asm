[bits 32]
global _start
extern kmain
extern _bss_start
extern _bss_end
extern _kstack_top

; Write a single byte in AL to COM1 (blocking)
%macro serial32 1
    mov al, %1
%%w:
    mov dx, 0x3FD
    in  al, dx
    test al, 0x20
    jz  %%w
    mov al, %1
    mov dx, 0x3F8
    out dx, al
%endmacro

_start:
    serial32 'P'    ; Protected mode loader reached

    mov ebp, _kstack_top
    mov esp, ebp

    serial32 'S'    ; Stack set

    xor eax, eax
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi
    rep stosb

    serial32 'B'    ; BSS cleared

    serial32 13
    serial32 10

    call kmain

    jmp $
