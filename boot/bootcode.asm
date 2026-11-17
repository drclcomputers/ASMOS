; The boot code, inside the kernel: Setup (apps/setup.c) and the shell's
; SYS command (fs/install.c) write it to the disks they make bootable.
; Built first by the makefile from mbr.asm, vbr.asm and stage2.asm.
section .rodata
global boot_mbr, boot_vbr, boot_stage2, boot_stage2_end
align 4
boot_mbr:        incbin "build/boot/mbr.bin"
boot_vbr:        incbin "build/boot/vbr.bin"
boot_stage2:     incbin "build/boot/stage2.bin"
boot_stage2_end:
