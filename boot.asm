[bits 16]
[org 0x7c00]

KERNEL_OFFSET equ 0x8000


jmp short start
nop

oem_name:           db "ASMOS   "      ; 8 bytes
bytes_per_sector:   dw 512
sectors_per_cluster:db 4               ; 4 * 512 = 2048 byte clusters
reserved_sectors:   dw 110             ; boot sector + 3 extra = 4
fat_count:          db 2
root_entry_count:   dw 512             ; 512 * 32 = 16384 bytes = 32 sectors
total_sectors_16:   dw 0               ; 0 = use total_sectors_32
media_type:         db 0xF8            ; fixed disk
sectors_per_fat:    dw 32              ; 32 * 512 = 16384 bytes -> covers 8192 clusters
sectors_per_track:  dw 63
head_count:         dw 16
hidden_sectors:     dd 0
total_sectors_32:   dd 65536           ; 65536 * 512 = 32MB image

drive_number:       db 0x80
reserved1:          db 0
boot_sig:           db 0x29            ; extended boot record signature
volume_id:          dd 0xDEADBEEF
volume_label:       db "ASMOS      "   ; 11 bytes
fs_type:            db "FAT16   "      ; 8 bytes

start:
    mov ax, 0x0013
    int 0x10

    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7c00

    mov [BOOT_DRIVE], dl

    mov ah, 0x02
    mov al, 100
    mov ch, 0x00
    mov dh, 0x00
    mov cl, 0x02
    mov dl, [BOOT_DRIVE]
    mov bx, KERNEL_OFFSET
    int 0x13

    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax

    jmp 0x08:init_pm

[bits 32]
init_pm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp KERNEL_OFFSET

gdt_start:
    dq 0x0
gdt_code:
    dw 0xffff, 0x0, 0x9a00, 0x00cf
gdt_data:
    dw 0xffff, 0x0, 0x9200, 0x00cf
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BOOT_DRIVE db 0

times 510-($-$$) db 0
dw 0xaa55
