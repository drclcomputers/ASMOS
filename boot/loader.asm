; Copyright (c) 2026 @drclcomputers. All rights reserved.
;
; This work is licensed under the terms of the MIT license.
; For a copy, see <https://opensource.org/licenses/MIT>.

[bits 32]
global _start
extern kmain
extern _bss_start
extern _bss_end
extern _kstack_top

_start:
    ; Our own GDT, in the kernel image. Stage 2 entered with its GDT, which
    ; lives in low memory (around 0x8460) that is free once the kernel runs:
    ; the floppy driver's DMA buffer is at 0x8000. Left in use, the first
    ; floppy track read overwrote it, and the next interrupt (the CPU reads
    ; CS's descriptor from the GDT) triple-faulted. Same selectors:
    ; 0x08 code, 0x10 data, flat 4 GB.
    lgdt [kernel_gdt_desc]
    jmp  0x08:.reload_cs
.reload_cs:
    ; Set all data segments to the flat data selector (GDT entry 2 = 0x10)
    mov  eax, 0x10
    mov  ds, eax
    mov  es, eax
    mov  fs, eax
    mov  gs, eax
    mov  ss, eax

    ; Set stack pointer to the top of the kernel stack region.
    ; _kstack_top is a linker symbol (value = 0x98000). The code runs at
    ; 1 MB: stage 2 read /SYSTEM/KERNEL.SYS there (boot/stage2.asm).
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

section .data
align 8
global kernel_gdt
kernel_gdt:
    dq 0                        ; null
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF ; 0x08: code, base 0, 4 GB, 32-bit
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF ; 0x10: data, base 0, 4 GB
    dq 0                        ; 0x18: APM 32-bit code   (lib/apm.c fills
    dq 0                        ; 0x20: APM 16-bit code    these three in,
    dq 0                        ; 0x28: APM data           in this order)
    dq 0                        ; 0x30
    dq 0                        ; 0x38
    dw 0x0FFF, 0x0400, 0x9200, 0x0040 ; 0x40: BIOS data area (0x400): some
                                ; APM BIOSes load selector 0x40 as if real
kernel_gdt_end:
kernel_gdt_desc:
    dw kernel_gdt_end - kernel_gdt - 1
    dd kernel_gdt
