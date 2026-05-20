[bits 32]
global _start
extern kmain
extern _bss_start
extern _bss_end
extern _kstack_top

_start:
    mov  eax, 0x10
    mov  ds, eax
    mov  es, eax
    mov  fs, eax
    mov  gs, eax
    mov  ss, eax
    mov  ebp, _kstack_top
    mov  esp, ebp

    ; ── Clear .bss ───────────────────────────────────────────────────────
    mov  edi, _bss_start
    mov  ecx, _bss_end
    sub  ecx, edi
    xor  eax, eax
    rep  stosb

    call kmain
    jmp  $
