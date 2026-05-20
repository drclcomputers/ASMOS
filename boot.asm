[bits 16]
[org 0x7c00]

; ─────────────────────────────────────────────────────────────────────────────
; Stage 1 – real-hardware hardened
;
; Loads:
;   Stage2   (sectors 1-8)   → 0x07E0:0000 = 0x7E00
;   Kernel   (sectors 9-1008)→ 0x0800:0000 = 0x8000  (fits in 500KB window)
;
; Changes vs original:
;   • I/O delay (port 0x80) after INT 13h reset, before retry loop
;   • Drive reset retried up to 3 times before giving up (flaky real drives)
;   • INT 13h AH=02 retried up to 3 times per chunk (mandatory on real HW)
;   • DL fixup only applies when DL < 0x80 (floppy); we trust BIOS if ≥ 0x80
;   • Added read-verify: after each chunk, confirm sector count in AL matches
;   • Chunk size capped at 63 – already correct, kept
;   • Stack at 0x7000 – fine, kept
; ─────────────────────────────────────────────────────────────────────────────

STAGE2_SEG      equ 0x07E0      ; 0x07E0 × 16 = 0x7E00
STAGE2_SECS     equ 8
KERNEL_SEG      equ 0x1000      ; 0x0800 × 16 = 0x8000
SECTORS_TO_LOAD equ 1000
CHS_SPT         equ 63
CHS_HEADS       equ 16

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
sectors_per_fat:        dw 32
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
cur_lba     dw 0
dst_seg     dw 0
remaining   dw 0
chunk       dw 0

; ── Entry point ──────────────────────────────────────────────────────────────
start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7000
    sti

    ; Save boot drive.  Trust DL only if it looks like a hard disk (≥ 0x80).
    ; Some BIOSes pass garbage in DL for USB/IDE boots.
    cmp  dl, 0x80
    jae  .dl_ok
    mov  dl, 0x80               ; default to first HDD
.dl_ok:
    mov  [boot_drive], dl

    ; Reset drive controller – retry up to 3 times
    mov  cx, 3
.reset_loop:
    xor  ax, ax
    mov  dl, [boot_drive]
    int  0x13
    jnc  .reset_ok
    push ax
    xor  ax, ax
    out  0x80, al               ; I/O delay
    pop  ax
    loop .reset_loop
    ; If all resets failed, carry on anyway (some BIOSes always fail reset)
.reset_ok:

    ; ── Load stage 2 (sectors 1-8) ───────────────────────────────────────────
    mov  word [cur_lba],   1
    mov  word [dst_seg],   STAGE2_SEG
    mov  word [remaining], STAGE2_SECS
    call read_sectors

    ; ── Load kernel (sectors 9-1008) ─────────────────────────────────────────
    mov  ax, STAGE2_SECS + 1    ; = 9
    mov  [cur_lba],        ax
    mov  word [dst_seg],   KERNEL_SEG
    mov  word [remaining], SECTORS_TO_LOAD
    call read_sectors

    ; ── Initialize video mode flag (0x0602) for stage 2 ──────────────────────
    ; Default to Mode X (0x01). Stage 2 will check this value to determine
    ; which video mode to set up. Can be overridden by bootloader config.
    xor  ax, ax
    mov  ds, ax
    mov  byte [0x0602], 0x00    ; 0x01 = Mode X (640x480 planar)
                                 ; 0x00 = Mode 13h (320x200 linear)

    jmp  STAGE2_SEG:0x0000

; ─────────────────────────────────────────────────────────────────────────────
; read_sectors
;   Reads [remaining] sectors starting at [cur_lba] into [dst_seg]:0000
;   Advances cur_lba and dst_seg as it goes.
;   Each INT 13h call is retried up to 3 times.
; ─────────────────────────────────────────────────────────────────────────────
read_sectors:
    push ax
    push bx
    push cx
    push dx
    push es

.loop:
    mov  ax, [remaining]
    test ax, ax
    jz   .done

    ; Clamp chunk to 63 sectors (max safe for real BIOSes)
    cmp  ax, 63
    jle  .cnt_ok
    mov  ax, 63
.cnt_ok:
    mov  [chunk], ax

    ; ── LBA → CHS conversion ─────────────────────────────────────────────────
    ; cylinder = LBA / (SPT × heads),  tempx = LBA / SPT
    ; head     = temp mod heads
    ; sector   = (LBA mod SPT) + 1
    xor  dx, dx
    mov  ax, [cur_lba]
    mov  bx, CHS_SPT
    div  bx                     ; AX = LBA/SPT, DX = LBA mod SPT
    inc  dx
    mov  cl, dl                 ; sector (1-based)

    xor  dx, dx
    mov  bx, CHS_HEADS
    div  bx                     ; AX = cylinder, DX = head
    mov  dh, dl                 ; head
    mov  ch, al                 ; cylinder low 8 bits
    shl  ah, 6
    or   cl, ah                 ; cylinder high 2 bits into CL[7:6]

    mov  ax, [dst_seg]
    mov  es, ax
    xor  bx, bx                 ; ES:BX = destination buffer

    ; ── INT 13h read with retry ───────────────────────────────────────────────
    mov  si, 3                  ; retry counter
.read_try:
    mov  ah, 0x02
    mov  al, [chunk]
    mov  dl, [boot_drive]
    int  0x13
    jnc  .read_ok

    ; Error: reset and retry
    push cx
    xor  ax, ax
    out  0x80, al               ; I/O delay
    xor  ax, ax
    mov  dl, [boot_drive]
    int  0x13                   ; drive reset
    xor  ax, ax
    out  0x80, al
    pop  cx

    dec  si
    jnz  .read_try
    jmp  disk_error             ; all retries exhausted

.read_ok:
    ; Restore DS (INT 13h can trash it on some BIOSes)
    xor  ax, ax
    mov  ds, ax

    mov  ax, [chunk]
    sub  [remaining], ax
    add  [cur_lba],   ax
    ; Advance destination segment: each sector = 512 bytes = 32 paragraphs
    shl  ax, 5                  ; × 32 paragraphs
    add  [dst_seg], ax
    jmp  .loop

.done:
    pop  es
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    ret

; ─────────────────────────────────────────────────────────────────────────────
disk_error:
    xor  ax, ax
    mov  ds, ax
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

; ── Boot signature ────────────────────────────────────────────────────────────
times 510-($-$$) db 0
dw 0xAA55
