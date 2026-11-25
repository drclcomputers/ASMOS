; Copyright (c) 2026 @drclcomputers. All rights reserved.
;
; This work is licensed under the terms of the MIT license.
; For a copy, see <https://opensource.org/licenses/MIT>.
;
; Shared by mbr.asm, vbr.asm and stage2.asm: where things are while booting.
;
;   0x0500  dword  RAM size in bytes (stage 2, INT 15h AH=88h), 0 = unknown
;   0x0600         MBR, moved here out of the way (gone once the VBR runs)
;   0x0602  byte   video mode (stage 2): 0 = 320x200x256, 1 = 640x400x16,
;                  2 = 640x480x16, 3 = 800x600x16, 4 = 320x200x16,
;                  5 = 640x400x256, 6 = 640x480x256, 7 = 800x600x256
;   0x0603  byte   the modes the machine allows (bit n = mode n)
;   0x0604  word   screen width, 0x0606 word height             (stage 2)
;   0x0608  byte   BIOS drive booted from (0x00 floppy, 0x80 disk)
;   0x0609  byte   1 = the BIOS did LBA reads (INT 13h AH=42h)
;   0x060C  dword  LBA of the volume booted from (0 on a floppy)
;   0x0610  word   APM version (BCD, 0x0102 = 1.2), 0 = none     (stage 2)
;   0x0612  word   APM flags, 0x0614 32-bit code segment, 0x0616 16-bit
;                  code segment, 0x0618 data segment (real-mode values),
;   0x061A  dword  entry point (offset in the 32-bit code segment)
;   0x0620  byte   'V': the video fields below are there         (stage 2)
;   0x0621  byte   flags: 1 a 486 or better, 2 the card refused 800x600x16,
;                  4 a VESA BIOS, 8 VBE 2.0+
;   0x0622  byte   the sizes with a usable 256-colour VESA mode (bit 1
;                  640x400, 2 640x480, 3 800x600)
;   0x0623  byte   the VESA mode in use: its window (0 A, 1 B)
;   0x0624  word   ... its number (0 = none), 0x0626 word bytes a line,
;   0x0628  dword  ... linear frame buffer (0 = banked), 0x062C word window
;                  granularity (KB), 0x062E word window size (KB), 0x0630
;                  word window segment
;   0x7C00         VBR: BPB at 0x0B..0x3D, then the values below, then code
;   0x7E00         stage 2 (the volume's reserved sectors after the VBR)
;   0x9000         stage 2: two FAT sectors, then one directory sector
;   0x10000        stage 2: 64 KB of kernel at a time, copied to 1 MB
;   0x100000       the kernel (/SYSTEM/KERNEL.SYS), entered in 32-bit mode
; ─────────────────────────────────────────────────────────────────────────────

BOOTINFO_RAM    equ 0x0500
BOOTINFO_VMODE  equ 0x0602
BOOTINFO_VMAX   equ 0x0603      ; the allowed modes
BOOTINFO_VMARK  equ 0x0620
BOOTINFO_VFLAGS equ 0x0621
VF_486          equ 0x01
VF_NO800        equ 0x02
VF_VBE          equ 0x04
VF_VBE2         equ 0x08
BOOTINFO_V256   equ 0x0622
BOOTINFO_VWIN   equ 0x0623
BOOTINFO_VBEMODE equ 0x0624
BOOTINFO_PITCH  equ 0x0626
BOOTINFO_LFB    equ 0x0628
BOOTINFO_GRAN   equ 0x062C
BOOTINFO_WINKB  equ 0x062E
BOOTINFO_WINSEG equ 0x0630
BOOTINFO_WIDTH  equ 0x0604
BOOTINFO_HEIGHT equ 0x0606
BOOTINFO_DRIVE  equ 0x0608
BOOTINFO_LBA    equ 0x0609
BOOTINFO_PART   equ 0x060C
BOOTINFO_APM    equ 0x0610
BOOTINFO_APMFL  equ 0x0612
BOOTINFO_APMCS  equ 0x0614
BOOTINFO_APMC16 equ 0x0616
BOOTINFO_APMDS  equ 0x0618
BOOTINFO_APMENT equ 0x061A

; the BPB, in the VBR at 0x7C00
BPB_BYTES_SEC   equ 0x7C0B
BPB_SEC_CLUS    equ 0x7C0D
BPB_RESERVED    equ 0x7C0E
BPB_FATS        equ 0x7C10
BPB_ROOT_ENTS   equ 0x7C11
BPB_TOTAL16     equ 0x7C13
BPB_FAT_SECS    equ 0x7C16
BPB_SPT         equ 0x7C18
BPB_HEADS       equ 0x7C1A
BPB_HIDDEN      equ 0x7C1C
BPB_TOTAL32     equ 0x7C20

; what the VBR found out about the BIOS, for stage 2 (right after the BPB)
VBR_DRIVE       equ 0x7C3E      ; byte: DL at boot
VBR_USE_LBA     equ 0x7C3F      ; byte: 1 = INT 13h extensions work
VBR_SPT         equ 0x7C40      ; word: sectors per track (CHS reads)
VBR_HEADS       equ 0x7C42      ; word: heads

STAGE2_ADDR     equ 0x7E00
STAGE2_MAX      equ 15          ; sectors: up to 0x9C00
FAT_BUF         equ 0x9000      ; 2 sectors (a FAT12 entry can straddle two)
DIR_BUF         equ 0x9400      ; 1 sector
LOAD_SEG        equ 0x1000      ; 0x10000, 64 KB aligned (ISA DMA)
KERNEL_ENTRY    equ 0x100000
