[bits 16]
[org 0x7c00]

; ─────────────────────────────────────────────────────────────────────────────
; Stage 1
;
; Loads:
;   Stage2   (sectors 1-8)   → 0x07E0:0000 = 0x7E00
;   Kernel   (sectors 9-1008)→ 0x1000:0000 = 0x10000
;
; Disk access, in order of preference:
;   1. INT 13h AH=42h extended read (plain LBA, no geometry at all) when
;      AH=41h says the BIOS supports it (most BIOSes from ~1995 on).
;   2. INT 13h AH=02h CHS read, using the geometry the BIOS reports with
;      AH=08h (the BPB values are only a last-resort fallback).
; Reads never cross a 64 KB physical boundary (ISA DMA) and, in CHS mode,
; never cross a track. Each read is retried 3 times with a reset between.
;
; The BIOS boot drive (DL) is kept as given and handed to the kernel at
; 0x0608 (drive number) / 0x0609 (1 = LBA reads worked).
; ─────────────────────────────────────────────────────────────────────────────

STAGE2_SEG      equ 0x07E0      ; 0x07E0 × 16 = 0x7E00
STAGE2_SECS     equ 8
KERNEL_SEG      equ 0x1000      ; 0x1000 × 16 = 0x10000
SECTORS_TO_LOAD equ 1000
BOOTINFO_DRIVE  equ 0x0608
BOOTINFO_LBA    equ 0x0609

; ── FAT16 BPB (required at offset 3 for BIOS to recognise the disk) ──────────
jmp short start
nop

oem_name:               db "ASMOS   "
bytes_per_sector:       dw 512
sectors_per_cluster:    db 4
reserved_sectors:       dw 1000
fat_count:              db 2
root_entry_count:       dw 512
total_sectors_16:       dw 0
media_type:             db 0xF8
sectors_per_fat:        dw 64          ; 64*256 entries covers all 16094 data clusters
sectors_per_track:      dw 63
head_count:             dw 16
hidden_sectors:         dd 0
total_sectors_32:       dd 65536
drive_number:           db 0x80
reserved1:              db 0
boot_sig:               db 0x29
volume_id:              dd 0xDEADBEEF
volume_label:           db "ASMOS      "
fs_type:                db "FAT16   "

; ── Stage-1 data (after BPB, still within boot sector) ───────────────────────
boot_drive  db 0x80
use_lba     db 0
cur_lba     dw 0
dst_seg     dw 0
remaining   dw 0
chunk       dw 0
dap:        db 0x10, 0          ; disk address packet for AH=42h
dap_count   dw 0
dap_off     dw 0
dap_seg     dw 0
dap_lba     dd 0, 0

; ── Entry point ──────────────────────────────────────────────────────────────
start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7000
    sti
    cld

    mov  [boot_drive], dl       ; trust the BIOS (0x00 = floppy/USB-FDD)

    ; LBA extensions present?
    mov  ah, 0x41
    mov  bx, 0x55AA
    int  0x13
    jc   .no_ext
    cmp  bx, 0xAA55
    jne  .no_ext
    test cl, 1                  ; "fixed disk access subset" (AH=42h)
    jz   .no_ext
    mov  byte [use_lba], 1
    jmp  .geo_done
.no_ext:
    ; CHS: ask the BIOS for its geometry instead of trusting the BPB
    mov  ah, 0x08
    mov  dl, [boot_drive]
    xor  di, di                 ; some BIOSes need ES:DI = 0 (ralf brown)
    int  0x13
    jc   .geo_done              ; keep the BPB values
    and  cx, 0x3F               ; sectors per track (bits 0-5)
    jz   .geo_done
    mov  [sectors_per_track], cx
    mov  dl, dh                 ; max head index
    xor  dh, dh
    inc  dx
    mov  [head_count], dx
.geo_done:
    xor  ax, ax
    mov  es, ax                 ; AH=08h changes ES

    ; ── Load stage 2 (sectors 1-8) ───────────────────────────────────────────
    mov  word [cur_lba],   1
    mov  word [dst_seg],   STAGE2_SEG
    mov  word [remaining], STAGE2_SECS
    call read_sectors

    ; ── Load kernel (sectors 9-1008) ─────────────────────────────────────────
    mov  word [cur_lba],   STAGE2_SECS + 1
    mov  word [dst_seg],   KERNEL_SEG
    mov  word [remaining], SECTORS_TO_LOAD
    call read_sectors

    ; ── Hand-off ─────────────────────────────────────────────────────────────
    mov  byte [0x0602], 0x00    ; video mode flag for stage 2 (see stage 2)
    mov  al, [boot_drive]
    mov  [BOOTINFO_DRIVE], al
    mov  al, [use_lba]
    mov  [BOOTINFO_LBA], al

    jmp  STAGE2_SEG:0x0000

; ─────────────────────────────────────────────────────────────────────────────
; read_sectors: [remaining] sectors from [cur_lba] to [dst_seg]:0000,
; advancing cur_lba / dst_seg.
; ─────────────────────────────────────────────────────────────────────────────
read_sectors:
.loop:
    mov  ax, [remaining]
    test ax, ax
    jz   .done
    cmp  ax, 63                 ; old BIOSes: at most 63..127 per call
    jbe  .c1
    mov  ax, 63
.c1:
    mov  bx, [dst_seg]          ; sectors until the next 64 KB boundary
    neg  bx
    and  bx, 0x0FFF
    jz   .c2                    ; segment is 64 KB aligned
    shr  bx, 5
    cmp  ax, bx
    jbe  .c2
    mov  ax, bx
.c2:
    mov  [chunk], ax
    mov  si, 3                  ; tries

.try:
    cmp  byte [use_lba], 0
    je   .chs
    mov  ax, [chunk]
    mov  [dap_count], ax
    mov  ax, [dst_seg]
    mov  [dap_seg], ax
    mov  ax, [cur_lba]
    mov  [dap_lba], ax
    push si
    mov  si, dap
    mov  ah, 0x42
    mov  dl, [boot_drive]
    int  0x13
    pop  si
    jmp  .result

.chs:
    xor  dx, dx
    mov  ax, [cur_lba]
    div  word [sectors_per_track]   ; AX = LBA / SPT, DX = LBA mod SPT
    mov  cx, [sectors_per_track]
    sub  cx, dx                     ; sectors left on this track
    cmp  cx, [chunk]
    jae  .t_ok
    mov  [chunk], cx
.t_ok:
    inc  dx
    mov  cl, dl                     ; sector (1-based)
    xor  dx, dx
    div  word [head_count]          ; AX = cylinder, DX = head
    mov  dh, dl
    mov  ch, al                     ; cylinder bits 0-7
    shl  ah, 6
    or   cl, ah                     ; cylinder bits 8-9
    mov  es, [dst_seg]
    xor  bx, bx
    mov  al, [chunk]
    mov  ah, 0x02
    mov  dl, [boot_drive]
    int  0x13

.result:
    jnc  .ok
    xor  ax, ax                     ; reset, then retry
    mov  dl, [boot_drive]
    int  0x13
    dec  si
    jnz  .try
    jmp  disk_error

.ok:
    xor  ax, ax                     ; some BIOSes trash DS
    mov  ds, ax
    mov  ax, [chunk]
    sub  [remaining], ax
    add  [cur_lba], ax
    shl  ax, 5                      ; 512 bytes = 32 paragraphs
    add  [dst_seg], ax
    jmp  .loop
.done:
    ret

; ─────────────────────────────────────────────────────────────────────────────
disk_error:
    mov  si, .msg
.p:
    lodsb
    test al, al
    jz   .h
    mov  ah, 0x0E
    xor  bh, bh
    int  0x10
    jmp  .p
.h:
    hlt
    jmp  .h
.msg db "Disk err", 0

; Keep 0x1BE-0x1FD (partition table) zero: some BIOSes validate it.
%if ($-$$) > 446
  %error "stage 1 code overlaps the partition table area"
%endif

; ── Boot signature ────────────────────────────────────────────────────────────
times 510-($-$$) db 0
dw 0xAA55
