[bits 32]
global _start
extern kmain
extern _bss_start
extern _bss_end
extern _kstack_top

_start:
    ; Set all data segments to the flat data selector (GDT entry 2 = 0x10)
    mov  eax, 0x10
    mov  ds, eax
    mov  es, eax
    mov  fs, eax
    mov  gs, eax
    mov  ss, eax

    ; Set stack pointer to the top of the kernel stack region.
    ; _kstack_top is a linker symbol (value = 0x98000). The code runs at
    ; 1 MB: stage 2 copied the image there (boot_stage2.asm).
    ; We want the VALUE of the symbol as an immediate, not a memory read.
    mov  esp, _kstack_top
    mov  ebp, esp

    ; Clear .bss — zero from _bss_start to _bss_end
    mov  edi, _bss_start
    mov  ecx, _bss_end
    sub  ecx, edi
    jz   .bss_done          ; skip if bss is empty (shouldn't happen, but safe)
    xor  eax, eax
    rep  stosb
.bss_done:

    ; Call the kernel — no return expected
    call kmain
    ; If kmain ever returns, halt forever
.halt:
    cli
    hlt
    jmp  .halt
