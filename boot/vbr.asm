; ─────────────────────────────────────────────────────────────────────────────
; ASMOS volume boot record (sector 0 of a FAT16 partition or a FAT12 floppy).
; Formatting writes the real BPB over the template at 0x0B..0x3D. Runs at
; 0x7C00: picks LBA or CHS reads, loads stage 2 (the reserved sectors) to
; 0x7E00 and jumps there, leaving the drive details at 0x7C3E.
; ─────────────────────────────────────────────────────────────────────────────
[bits 16]
[org 0x7C00]

%include "boot/bootdefs.inc"

    jmp  short start
    nop
oem_name:           db "ASMOS1.0"
bytes_per_sector:   dw 512
sectors_per_cluster:db 1
reserved_sectors:   dw 16           ; this sector + stage 2
fat_count:          db 2
root_entry_count:   dw 224
total_sectors_16:   dw 2880
media_type:         db 0xF0
sectors_per_fat:    dw 9
sectors_per_track:  dw 18
head_count:         dw 2
hidden_sectors:     dd 0
total_sectors_32:   dd 0
drive_number:       db 0
reserved1:          db 0
boot_sig:           db 0x29
volume_id:          dd 0
volume_label:       db "ASMOS      "
fs_type:            db "FAT12   "
; 0x3E: filled in at boot, read by stage 2 (bootdefs.inc: VBR_*)
vbr_drive:          db 0
vbr_use_lba:        db 0
vbr_spt:            dw 0
vbr_heads:          dw 0

start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00
    sti
    cld
    mov  [vbr_drive], dl
    mov  ax, [sectors_per_track] ; a floppy: the BPB is right
    mov  [vbr_spt], ax
    mov  ax, [head_count]
    mov  [vbr_heads], ax

    mov  ah, 0x41
    mov  bx, 0x55AA
    int  0x13
    jc   .no_ext
    cmp  bx, 0xAA55
    jne  .no_ext
    test cl, 1
    jz   .no_ext
    mov  byte [vbr_use_lba], 1
    jmp  .read
.no_ext:
    test byte [vbr_drive], 0x80
    jz   .read                  ; floppy: keep the BPB geometry
    mov  ah, 0x08               ; hard disk: the BIOS's geometry
    mov  dl, [vbr_drive]
    xor  di, di
    int  0x13
    jc   .read
    and  cx, 0x3F
    jz   .read
    mov  [vbr_spt], cx
    movzx ax, dh
    inc  ax
    mov  [vbr_heads], ax
.read:
    xor  ax, ax
    mov  es, ax
    mov  cx, [reserved_sectors] ; stage 2: the rest of the reserved area
    dec  cx
    jz   no_stage2
    cmp  cx, STAGE2_MAX
    jbe  .n_ok
    mov  cx, STAGE2_MAX
.n_ok:
    mov  eax, [hidden_sectors]
    inc  eax
    mov  bx, STAGE2_ADDR
.next:
    call read1
    inc  eax
    add  bx, 512
    loop .next
    cmp  word [STAGE2_ADDR + 2], 'VM' ; stage 2 starts: jmp short, "VM"
    jne  no_stage2
    mov  dl, [vbr_drive]
    jmp  0:STAGE2_ADDR

; read1: sector EAX (on the disk) to ES:BX (ES = 0); 3 tries, else a message
read1:
    pushad
    mov  [r_lba], eax
    mov  [r_off], bx
    mov  di, 3
.try:
    mov  eax, [r_lba]
    cmp  byte [vbr_use_lba], 0
    je   .chs
    mov  [dap_lba], eax
    mov  ax, [r_off]
    mov  [dap_off], ax
    mov  si, dap
    mov  ah, 0x42
    mov  dl, [vbr_drive]
    int  0x13
    jmp  .result
.chs:
    xor  edx, edx
    movzx ecx, word [vbr_spt]
    div  ecx                    ; eax = track, edx = sector - 1
    mov  cl, dl
    inc  cl
    xor  edx, edx
    movzx esi, word [vbr_heads]
    div  esi                    ; eax = cylinder, edx = head
    mov  dh, dl
    mov  ch, al
    shl  ah, 6
    or   cl, ah
    mov  bx, [r_off]
    mov  ax, 0x0201
    mov  dl, [vbr_drive]
    int  0x13
.result:
    jnc  .ok
    xor  ax, ax                 ; reset, try again
    mov  dl, [vbr_drive]
    int  0x13
    dec  di
    jnz  .try
    mov  si, msg_disk
    jmp  fail
.ok:
    popad
    ret

no_stage2:
    mov  si, msg_nos2
fail:
    lodsb
    test al, al
    jz   .key
    mov  ah, 0x0E
    mov  bx, 7
    int  0x10
    jmp  fail
.key:
    xor  ax, ax                 ; a key, then the BIOS tries the next device
    int  0x16
    int  0x19

msg_disk db "Disk error. Press a key", 0
msg_nos2 db "Not an ASMOS disk. Press a key", 0

r_lba    dd 0
r_off    dw 0
dap:     db 0x10, 0
         dw 1
dap_off  dw 0
         dw 0
dap_lba  dd 0, 0

%if ($-$$) > 510
  %error "VBR over 510 bytes"
%endif
times 510-($-$$) db 0
dw 0xAA55
