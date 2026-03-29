[bits 16]
[org 0x7c00]

KERNEL_OFFSET equ 0x8000

start:
    mov ax, 0x0013
    int 0x10

    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7c00

    mov [BOOT_DRIVE], dl

    mov ah, 0x02      ; BIOS Read Sector
    mov al, 100       ; Read 100 sectors
    mov ch, 0x00      ; Cylinder 0
    mov dh, 0x00      ; Head 0
    mov cl, 0x02      ; Start reading at Sector 2
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
