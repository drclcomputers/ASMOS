[bits 16]
[org 0x7c00]

; ── constants ────────────────────────────────────────────────────────────────
KERNEL_OFFSET   equ 0x8000
KERNEL_SEGMENT  equ 0x0800
SECTORS_TO_LOAD equ 1000
VESA_MODE       equ 0x0100 ; 0x0146 - 320x200 - mode 13h - QEMU only

jmp short start
nop

; ── FAT16 Boot Parameter Block ──────────────────────────────────────────────
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

; ── Disk Address Packet (for LBA reads) ──────────────────────────────────────
dap:
    db  0x10
    db  0x00
    dap_count:  dw  0
    dap_off:    dw  0
    dap_seg:    dw  0
    dap_lba_lo: dd  0
    dap_lba_hi: dd  0

; ── entry point ──────────────────────────────────────────────────────────────
start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    mov byte [BOOT_DRIVE], 0x80

    ; --- VESA: get mode info & store LFB address ----------------------------
    xor ax, ax
    mov es, ax
    mov di, 0x0700
    mov ax, 0x4F01
    mov cx, VESA_MODE
    int 0x10
    cmp ax, 0x004F
    jne .skip_lfb
    mov eax, [es:di + 40]       ; PhysBasePtr (LFB address)
    mov [0x0600], eax
.skip_lfb:

    ; --- set VESA linear frame buffer mode ----------------------------------
    mov ax, 0x4F02
    mov bx, 0x4000 | VESA_MODE  ; bit 14 = linear framebuffer
    int 0x10
    cmp ax, 0x004F
    jne vesa_error

    ; success – skip over the error handler
    jmp lba_check

vesa_error:
    mov si, msg_vesa_err
    jmp print_hang
msg_vesa_err db "VESA mode error", 0

    ; --- LBA extension check -------------------------------------------------
lba_check:
    xor ax, ax
    mov es, ax

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc  no_lba
    cmp bx, 0xAA55
    jne no_lba

    ; --- load kernel in 127-sector chunks -----------------------------------
    mov bx, SECTORS_TO_LOAD      ; sectors remaining
    mov cx, 1                    ; starting LBA
    mov di, KERNEL_SEGMENT       ; destination segment

.load_chunk:
    mov ax, 127
    cmp bx, ax
    jle .use_remaining
    jmp .chunk_sized
.use_remaining:
    mov ax, bx

.chunk_sized:
    mov [dap_count], ax
    mov word [dap_off], 0
    mov [dap_seg], di
    mov word [dap_lba_lo],     cx
    mov word [dap_lba_lo + 2], 0
    mov dword [dap_lba_hi],    0

    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    mov si, dap
    int 0x13
    jc  disk_error

    mov ax, [dap_count]
    sub bx, ax
    add cx, ax
    shl ax, 5                   ; ax * 32 (= bytes per sector / 16)
    add di, ax

    test bx, bx
    jnz .load_chunk

    ; --- enter protected mode ------------------------------------------------
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or  eax, 0x1
    mov cr0, eax
    jmp 0x08:init_pm

    ; --- 32-bit protected mode -----------------------------------------------
[bits 32]
init_pm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp KERNEL_OFFSET

    ; --- 16-bit error handlers -----------------------------------------------
[bits 16]
no_lba:
    mov si, msg_no_lba
    jmp print_hang

disk_error:
    mov si, msg_disk_err

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

msg_no_lba   db "No LBA support", 0
msg_disk_err db "Disk read error", 0

; ── GDT ──────────────────────────────────────────────────────────────────────
gdt_start:
    dq 0x0
gdt_code:
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
gdt_data:
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BOOT_DRIVE db 0

; ── boot signature ───────────────────────────────────────────────────────────
times 510-($-$$) db 0
dw 0xAA55
