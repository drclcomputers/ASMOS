; Copyright (c) 2026 @drclcomputers. All rights reserved.
;
; This work is licensed under the terms of the MIT license.
; For a copy, see <https://opensource.org/licenses/MIT>.
;
; The real-mode half of a BIOS call from the kernel (lib/realmode.asm copies
; it to 0x1000 and comes here through the 16-bit selector 0x30). Paging and
; protection off, the interrupt through the real-mode vector table, then
; protected mode again and a far jump back to the kernel (RM_BACK).

[bits 16]
[org 0x1000]

%include "boot/rmdefs.inc"

entry16:                        ; 16-bit protected mode, CS = 0x30 (base 0)
    mov  ax, 0x38               ; 64 KB data segments, as real mode wants
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  eax, cr0
    and  eax, 0x7FFFFFFE        ; paging and protection off
    mov  cr0, eax
    jmp  0x0000:real

real:
    xor  ax, ax
    mov  ds, ax
    mov  ss, ax
    mov  sp, RM_STACK
    lidt [rm_idt]
    movzx bx, byte [RM_INTNO]   ; the vector, from the real-mode table
    shl  bx, 2
    mov  eax, [bx]
    mov  [RM_VEC], eax
    mov  eax, [RM_REGS + 0]
    mov  ebx, [RM_REGS + 4]
    mov  ecx, [RM_REGS + 8]
    mov  edx, [RM_REGS + 12]
    mov  esi, [RM_REGS + 16]
    mov  edi, [RM_REGS + 20]
    mov  es, [RM_REGS + 26]
    mov  ds, [RM_REGS + 24]     ; (last: cs: from here on)
    pushf                       ; as INT n does
    call far [cs:RM_VEC]
    pushf
    mov  [cs:RM_REGS + 0], eax
    mov  [cs:RM_REGS + 4], ebx
    mov  [cs:RM_REGS + 8], ecx
    mov  [cs:RM_REGS + 12], edx
    mov  [cs:RM_REGS + 16], esi
    mov  [cs:RM_REGS + 20], edi
    mov  [cs:RM_REGS + 24], ds
    mov  [cs:RM_REGS + 26], es
    pop  word [cs:RM_REGS + 28] ; the flags
    cli
    mov  eax, cr0               ; protection on (the GDT is where it was)
    or   al, 1
    mov  cr0, eax
    jmp  far dword [cs:RM_BACK] ; 0x08:back32, in the kernel

rm_idt:
    dw   0x03FF
    dd   0

%if ($ - $$) > RM_REGS - RM_BASE - 0x200
  %error "the real-mode stub leaves too little stack"
%endif
