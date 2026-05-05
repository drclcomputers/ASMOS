[bits 16]
[org 0x7c00]

KERNEL_OFFSET   equ 0x8000
KERNEL_SEGMENT  equ 0x0800
SECTORS_TO_LOAD equ 1000
DEFAULT_MODE    equ 0x0100

jmp short start
nop

; ─── FAT16 BPB ──────────────────────────────────────────────────────────────
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

; ─── DAP ─────────────────────────────────────────────────────────────────────
dap:
    db  0x10, 0x00
dap_count:  dw 0
dap_off:    dw 0
dap_seg:    dw 0
dap_lba_lo: dd 0
dap_lba_hi: dd 0

; ─── Entry ───────────────────────────────────────────────────────────────────
start:
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7c00
    mov  byte [BOOT_DRIVE], 0x80

    ; ── Enumerate VESA modes into table at 0x060C ────────────────────────────
    ; Get VBE controller info at 0x0700
    mov  ax, 0x4F00
    mov  di, 0x0700
    int  0x10
    cmp  ax, 0x004F
    jne  no_vesa

    ; Mode list far pointer at VBEInfo+14 (off=0x070E, seg=0x0710)
    mov  si, [0x070E]
    mov  ax, [0x0710]
    mov  fs, ax

    mov  di, 0x060C          ; table destination
    xor  cx, cx              ; count

.enum_loop:
    cmp  cx, 8
    jge  .enum_done
    mov  bx, [fs:si]
    cmp  bx, 0xFFFF
    je   .enum_done
    add  si, 2

    ; Get ModeInfo at 0x0700
    push si
    push cx
    push bx
    push di
    push es
    xor  ax, ax
    mov  es, ax
    mov  ax, 0x4F01
    mov  cx, bx
    mov  di, 0x0700
    int  0x10
    pop  es
    pop  di
    pop  bx
    pop  cx
    pop  si
    cmp  ax, 0x004F
    jne  .enum_next

    ; Filter: 8bpp only
    cmp  byte [0x0719], 8    ; BitsPerPixel @ offset 25
    jne  .enum_next
    ; Filter: linear framebuffer supported (attr bit 7)
    test byte [0x0700], 0x80 ; ModeAttributes @ offset 0
    jz   .enum_next

    ; Store: mode_number(2), width(2), height(2), bpp(1), pad(1), fb_base(4)
    mov  ax, bx
    stosw
    mov  ax, [0x0712]        ; XResolution @ offset 18
    stosw
    mov  ax, [0x0714]        ; YResolution @ offset 20
    stosw
    mov  al, 8
    stosb
    xor  al, al
    stosb
    mov  eax, [0x0728]       ; PhysBasePtr @ offset 40
    stosd
    inc  cx

.enum_next:
    jmp  .enum_loop

.enum_done:
    mov  [0x060A], cx        ; store mode count

    ; ── Pick mode to set ─────────────────────────────────────────────────────
    ; Default: first entry from table (or DEFAULT_MODE if table empty)
    mov  bx, DEFAULT_MODE

    cmp  word [0x07F2], 0xA5B6   ; reboot-with-mode magic?
    jne  .set_mode

    ; Clear magic
    mov  word [0x07F2], 0

    ; Index is in [0x07F0]; look up mode number from table
    movzx ax, byte [0x07F0]
    cmp  ax, cx
    jge  .set_mode           ; out of range → use default
    ; table entry = 0x060C + ax*12, mode_number is first word
    mov  bx, ax
    mov  ax, 12
    mul  bx
    mov  bx, ax
    add  bx, 0x060C
    mov  bx, [bx]            ; mode_number

.set_mode:
    ; Get ModeInfo for chosen mode → store fb/width/height/bpp at 0x0600..0x0609
    push es
    xor  ax, ax
    mov  es, ax
    mov  ax, 0x4F01
    mov  cx, bx
    mov  di, 0x0700
    int  0x10
    pop  es
    cmp  ax, 0x004F
    jne  no_vesa

    mov  eax, [0x0728]       ; PhysBasePtr
    mov  [0x0600], eax
    mov  ax, [0x0712]        ; XResolution
    mov  [0x0604], ax
    mov  ax, [0x0714]        ; YResolution
    mov  [0x0606], ax
    mov  al, [0x0719]        ; BitsPerPixel
    mov  [0x0608], al

    ; Set the VESA mode (LFB bit 14)
    mov  ax, 0x4F02
    or   bx, 0x4000
    int  0x10
    cmp  ax, 0x004F
    jne  no_vesa

    ; ── Load kernel ──────────────────────────────────────────────────────────
    mov  bx, SECTORS_TO_LOAD
    mov  cx, 1
    mov  di, KERNEL_SEGMENT

.load_chunk:
    mov  ax, 127
    cmp  bx, ax
    jle  .use_remaining
    jmp  .chunk_sized
.use_remaining:
    mov  ax, bx
.chunk_sized:
    mov  [dap_count], ax
    mov  word [dap_off], 0
    mov  [dap_seg], di
    mov  word [dap_lba_lo], cx
    mov  word [dap_lba_lo + 2], 0
    mov  dword [dap_lba_hi], 0

    mov  ah, 0x42
    mov  dl, [BOOT_DRIVE]
    mov  si, dap
    int  0x13
    jc   disk_error

    mov  ax, [dap_count]
    sub  bx, ax
    add  cx, ax
    shl  ax, 5
    add  di, ax
    test bx, bx
    jnz  .load_chunk

    ; ── Enter protected mode ─────────────────────────────────────────────────
    cli
    lgdt [gdt_descriptor]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:init_pm

[bits 32]
init_pm:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x90000
    jmp  KERNEL_OFFSET

[bits 16]
no_vesa:
disk_error:
    mov  si, msg_err
print_hang:
    lodsb
    test al, al
    jz   hang
    mov  ah, 0x0E
    xor  bh, bh
    int  0x10
    jmp  print_hang
hang:
    hlt
    jmp  hang

msg_err db "Boot error",0

gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
gdt_data:
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BOOT_DRIVE db 0

times 510-($-$$) db 0
dw 0xAA55
