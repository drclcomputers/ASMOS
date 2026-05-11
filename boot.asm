[bits 16]
[org 0x7c00]

; Stage1: loads stage2 (sectors 1-8) to 0x07E0:0000 (=0x7E00),
;         loads kernel (sectors 9+)  to 0x0800:0000 (=0x8000),
;         jumps to stage2.

STAGE2_SEG      equ 0x07E0      ; 0x07E0 * 16 = 0x7E00
STAGE2_SECS     equ 8
KERNEL_SEG      equ 0x0800      ; 0x0800 * 16 = 0x8000
SECTORS_TO_LOAD equ 1000
CHS_SPT         equ 63
CHS_HEADS       equ 16

jmp short start
nop

; FAT16 BPB
oem_name:           db "ASMOS   "
bytes_per_sector:   dw 512
sectors_per_cluster:db 4
reserved_sectors:   dw 1000
fat_count:          db 2
root_entry_count:   dw 512
total_sectors_16:   dw 0
media_type:         db 0xF8
sectors_per_fat:    dw 32
sectors_per_track:  dw 63
head_count:         dw 16
hidden_sectors:     dd 0
total_sectors_32:   dd 65536
drive_number:       db 0x80
reserved1:          db 0
boot_sig:           db 0x29
volume_id:          dd 0xDEADBEEF
volume_label:       db "ASMOS      "
fs_type:            db "FAT16   "

boot_drive  db 0x80
cur_lba     dw 0
dst_seg     dw 0
remaining   dw 0
chunk       dw 0        ; <-- data NOT inside the subroutine

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    sti
    mov [boot_drive], dl

    ; Reset drive + fix DL if BIOS corrupted it
    xor ax, ax
    mov dl, [boot_drive]
    int 0x13
    cmp dl, 0x80
    jae .dl_ok
    mov dl, 0x80
    mov [boot_drive], dl
.dl_ok:

    ; --- load stage2 to 0x07E0:0000 = 0x7E00 ---
    mov word [cur_lba],   1
    mov word [dst_seg],   STAGE2_SEG
    mov word [remaining], STAGE2_SECS
    call read_sectors

    ; --- load kernel to 0x0800:0000 = 0x8000 ---
    mov ax, STAGE2_SECS + 1
    mov [cur_lba],        ax
    mov word [dst_seg],   KERNEL_SEG
    mov word [remaining], SECTORS_TO_LOAD
    call read_sectors

    jmp STAGE2_SEG:0x0000

; Reads [remaining] sectors from [cur_lba] into [dst_seg]:0000
read_sectors:
    push ax
    push bx
    push cx
    push dx
    push es
.loop:
    mov ax, [remaining]
    test ax, ax
    jz  .done

    cmp ax, 63
    jle .cnt_ok
    mov ax, 63
.cnt_ok:
    mov [chunk], ax

    ; LBA → CHS
    xor dx, dx
    mov ax, [cur_lba]
    mov bx, CHS_SPT
    div bx
    inc dx
    mov cl, dl          ; sector (1-based)

    xor dx, dx
    mov bx, CHS_HEADS
    div bx
    mov dh, dl          ; head
    mov ch, al          ; cylinder low 8 bits
    shl ah, 6
    or  cl, ah          ; cylinder high 2 bits into cl[7:6]

    mov ax, [dst_seg]
    mov es, ax
    xor bx, bx          ; ES:BX = destination

    mov ah, 0x02
    mov al, [chunk]
    mov dl, [boot_drive]
    int 0x13
    jc  disk_error

    xor ax, ax
    mov ds, ax

    mov ax, [chunk]
    sub [remaining], ax
    add [cur_lba],   ax
    ; advance dst_seg by sectors*32 (each sector = 512 bytes = 32 paragraphs)
    shl ax, 5
    add [dst_seg], ax
    jmp .loop
.done:
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

disk_error:
    xor ax, ax
    mov ds, ax
    mov si, .msg
.p: lodsb
    test al, al
    jz .h
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    jmp .p
.h: hlt
    jmp .h
.msg db "Disk err",0

times 510-($-$$) db 0
dw 0xAA55
