; ─────────────────────────────────────────────────────────────────────────────
; Shared by mbr.asm, vbr.asm and stage2.asm: where things are while booting.
;
;   0x0500  dword  RAM size in bytes (stage 2, INT 15h AH=88h), 0 = unknown
;   0x0600         MBR, moved here out of the way (gone once the VBR runs)
;   0x0602  byte   video mode: 0 = 320x200x256, 1 = 640x400x16  (stage 2)
;   0x0604  word   screen width, 0x0606 word height             (stage 2)
;   0x0608  byte   BIOS drive booted from (0x00 floppy, 0x80 disk)
;   0x0609  byte   1 = the BIOS did LBA reads (INT 13h AH=42h)
;   0x060C  dword  LBA of the volume booted from (0 on a floppy)
;   0x0610  word   APM version (BCD, 0x0102 = 1.2), 0 = none     (stage 2)
;   0x0612  word   APM flags, 0x0614 32-bit code segment, 0x0616 16-bit
;                  code segment, 0x0618 data segment (real-mode values),
;   0x061A  dword  entry point (offset in the 32-bit code segment)
;   0x7C00         VBR: BPB at 0x0B..0x3D, then the values below, then code
;   0x7E00         stage 2 (the volume's reserved sectors after the VBR)
;   0x9000         stage 2: two FAT sectors, then one directory sector
;   0x10000        stage 2: 64 KB of kernel at a time, copied to 1 MB
;   0x100000       the kernel (/SYSTEM/KERNEL.SYS), entered in 32-bit mode
; ─────────────────────────────────────────────────────────────────────────────

BOOTINFO_RAM    equ 0x0500
BOOTINFO_VMODE  equ 0x0602
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
