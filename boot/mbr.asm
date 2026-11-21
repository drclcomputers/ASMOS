; ─────────────────────────────────────────────────────────────────────────────
; ASMOS master boot record. Moves itself to 0x0600, loads the active
; partition's VBR to 0x7C00 (LBA, else CHS) and jumps to it with DL = drive
; and DS:SI = its table entry, like DOS. Never changes the partition table.
; ─────────────────────────────────────────────────────────────────────────────
[bits 16]
[org 0x0600]

%include "boot/bootdefs.inc"

start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00
    sti
    cld
    mov  si, 0x7C00             ; out of the VBR's way
    mov  di, 0x0600
    mov  cx, 256
    rep  movsw
    jmp  0:moved

moved:
    mov  [drive], dl
    mov  si, table
    mov  cx, 4
.find:
    cmp  byte [si], 0x80
    je   found
    add  si, 16
    loop .find
    mov  si, msg_none
    jmp  fail

found:
    mov  [entry], si
    mov  eax, [si + 8]          ; the partition's first sector
    mov  [dap_lba], eax

    mov  ah, 0x41               ; LBA extensions?
    mov  bx, 0x55AA
    mov  dl, [drive]
    int  0x13
    jc   .chs
    cmp  bx, 0xAA55
    jne  .chs
    test cl, 1
    jz   .chs
    mov  di, 3
.lba_try:
    mov  si, dap
    mov  ah, 0x42
    mov  dl, [drive]
    int  0x13
    jnc  loaded
    call reset
    jnz  .lba_try
    jmp  disk_fail

.chs:
    mov  ah, 0x08               ; geometry
    mov  dl, [drive]
    xor  di, di
    int  0x13
    mov  bx, 0
    mov  es, bx
    jc   .entry_chs
    and  cx, 0x3F
    jz   .entry_chs
    movzx ecx, cx               ; sectors per track
    movzx ebx, dh
    inc  ebx                    ; heads
    mov  eax, [dap_lba]
    xor  edx, edx
    div  ecx                    ; eax = track, edx = sector - 1
    inc  dx
    mov  [sect], dl
    xor  edx, edx
    div  ebx                    ; eax = cylinder, edx = head
    cmp  eax, 1023
    ja   disk_fail              ; past what CHS can reach
    mov  ch, al
    shl  ah, 6
    or   ah, [sect]
    mov  cl, ah
    mov  dh, dl
    jmp  .chs_go
.entry_chs:                     ; no geometry: the entry's own CHS, as DOS
    mov  si, [entry]
    mov  dh, [si + 1]
    mov  cx, [si + 2]
.chs_go:
    mov  di, 3
.chs_try:
    push cx
    push dx
    mov  bx, 0x7C00
    mov  ax, 0x0201
    mov  dl, [drive]
    int  0x13
    pop  dx
    pop  cx
    jnc  loaded
    call reset
    jnz  .chs_try
    jmp  disk_fail

loaded:
    cmp  word [0x7DFE], 0xAA55
    jne  .not_sys
    mov  si, [entry]
    mov  dl, [drive]
    jmp  0:0x7C00
.not_sys:
    mov  si, msg_nosys
    jmp  fail

; reset the drive between tries; ZF set when DI runs out
reset:
    xor  ax, ax
    mov  dl, [drive]
    int  0x13
    dec  di
    ret

disk_fail:
    mov  si, msg_disk
fail:
    lodsb
    test al, al
    jz   .halt
    mov  ah, 0x0E
    mov  bx, 7
    int  0x10
    jmp  fail
.halt:
    hlt
    jmp  .halt

msg_none  db "No active partition", 0
msg_disk  db "Disk error", 0
msg_nosys db "Partition not bootable", 0

drive db 0
sect  db 0
entry dw 0
dap:  db 0x10, 0
      dw 1                      ; one sector
      dw 0x7C00, 0              ; to 0:7C00
dap_lba dd 0, 0

%if ($-$$) > 0x1B8
  %error "MBR code runs into the disk signature / partition table"
%endif
times 0x1B8-($-$$) db 0
disk_sig dd 0                   ; Setup puts a serial number here
         dw 0
table:   times 64 db 0          ; the partition table (Setup / mkdisk.py)
         dw 0xAA55
