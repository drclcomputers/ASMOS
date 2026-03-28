[bits 16]
[org 0x7c00]

KERNEL_OFFSET equ 0x8000

start:
    ; 1. Set Video Mode 13h
    mov ax, 0x0013
    int 0x10

    ; 2. Set up a basic 16-bit stack
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7c00

    mov [BOOT_DRIVE], dl ; Save the drive BIOS booted from

    ; 3. Read the kernel from disk into KERNEL_OFFSET (0x8000)
    mov ah, 0x02      ; BIOS Read Sector
    mov al, 15        ; Read 15 sectors (plenty of room for our C code)
    mov ch, 0x00      ; Cylinder 0
    mov dh, 0x00      ; Head 0
    mov cl, 0x02      ; Start reading at Sector 2
    mov dl, [BOOT_DRIVE]
    mov bx, KERNEL_OFFSET
    int 0x13

    ; 4. Switch to 32-bit Protected Mode
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax

    jmp 0x08:init_pm  ; Far jump to flush the pipeline

[bits 32]
init_pm:
    ; 5. Set up 32-bit segment registers
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; 6. Jump to the C Kernel we just loaded!
    jmp KERNEL_OFFSET

; --- GDT ---
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

; Bootsector magic padding
times 510-($-$$) db 0
dw 0xaa55
