; Copyright (c) 2026 @drclcomputers. All rights reserved.
;
; This work is licensed under the terms of the MIT license.
; For a copy, see <https://opensource.org/licenses/MIT>.
;
; BIOS calls from protected mode (lib/realmode.h): down to real mode through
; the stub at 0x1000 (boot/rmstub.asm), the interrupt, and back. With every
; IRQ masked meanwhile: the BIOS's vectors aren't ours. Selectors 0x30 and
; 0x38 are 16-bit code and data, base 0 (boot/loader.asm).
;

[bits 32]

%include "boot/rmdefs.inc"

extern g_kernel_pd_phys

section .rodata
rm_stub:
    incbin "build/boot/rmstub.bin"
rm_stub_end:

section .data
align 4
rm_buf:   times 8 dd 0          ; the registers, on the way (kernel data)
save_esp  dd 0
save_cr0  dd 0
save_cr3  dd 0
save_idt  dw 0
          dd 0
save_m1   db 0
save_m2   db 0

section .text

; void realmode_install(void): the stub to 0x1000 (paging still off, or
; low memory mapped as it is)
global realmode_install
realmode_install:
    push esi
    push edi
    cld
    mov  esi, rm_stub
    mov  edi, RM_BASE
    mov  ecx, rm_stub_end - rm_stub
    rep  movsb
    mov  dword [RM_BACK], back32
    mov  word [RM_BACK + 4], 0x08
    pop  edi
    pop  esi
    ret

; void realmode_int(uint8_t n, rm_regs_t *r): INT n with r's registers in,
; and out
global realmode_int
realmode_int:
    push ebp
    mov  ebp, esp
    pushad
    pushfd
    cli
    cld
    mov  esi, [ebp + 12]        ; (the caller's space) the registers
    mov  edi, rm_buf
    mov  ecx, 8
    rep  movsd
    mov  eax, cr3               ; the kernel's page tables: an app's may
    mov  [save_cr3], eax        ; not map low memory
    mov  eax, [g_kernel_pd_phys]
    test eax, eax
    jz   .space_ok
    mov  cr3, eax
.space_ok:
    in   al, 0x21               ; every IRQ masked
    mov  [save_m1], al
    in   al, 0xA1
    mov  [save_m2], al
    mov  al, 0xFF
    out  0xA1, al
    out  0x21, al
    mov  esi, rm_buf
    mov  edi, RM_REGS
    mov  ecx, 8
    rep  movsd
    mov  eax, [ebp + 8]
    mov  [RM_INTNO], al
    mov  [save_esp], esp
    sidt [save_idt]
    mov  eax, cr0
    mov  [save_cr0], eax
    jmp  0x30:RM_BASE

back32:                         ; protected mode again, paging still off
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, [save_esp]
    cld
    mov  esi, RM_REGS
    mov  edi, rm_buf
    mov  ecx, 8
    rep  movsd
    mov  eax, [save_cr3]
    mov  cr3, eax
    mov  eax, [save_cr0]        ; paging on, as it was
    mov  cr0, eax
    jmp  .flush                 ; (a 386: the prefetch queue)
.flush:
    lidt [save_idt]
    mov  al, [save_m2]
    out  0xA1, al
    mov  al, [save_m1]
    out  0x21, al
    mov  esi, rm_buf            ; (the caller's space again)
    mov  edi, [ebp + 12]
    mov  ecx, 8
    rep  movsd
    popfd
    popad
    pop  ebp
    ret
