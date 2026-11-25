; Copyright (c) 2026 @drclcomputers. All rights reserved.
;
; This work is licensed under the terms of the MIT license.
; For a copy, see <https://opensource.org/licenses/MIT>.
;
; BIOS calls from the kernel (lib/realmode.asm, boot/rmstub.asm): the page
; of low memory they use. Nothing else uses 0x1000-0x1FFF once the kernel
; runs (the floppy's DMA buffers start at 0x7000, the back buffer at
; 0x10000).

RM_BASE   equ 0x1000            ; the stub's code
RM_STACK  equ 0x1DF0            ; its stack, down from here
RM_REGS   equ 0x1E00            ; eax ebx ecx edx esi edi, ds es flags
RM_INTNO  equ 0x1E20            ; byte: the interrupt
RM_VEC    equ 0x1E24            ; dword: its vector (segment:offset)
RM_BACK   equ 0x1E28            ; fword: where the kernel goes on (0x08:)
