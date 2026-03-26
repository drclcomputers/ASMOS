BITS 16
ORG 0x7C00

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [BOOT_DRIVE], dl ; Save drive ID BIOS gave

    ; Reset Disk System
    mov ah, 0
    mov dl, [BOOT_DRIVE] ; Use saved drive ID
    int 0x13

    ; Read Stage 2 from Disk
    mov ax, 0x0000      ; Reset ES
    mov es, ax
    mov bx, 0x8000      ; ES:BX = 0x0000:0x8000

    mov ah, 0x02        ; BIOS read sector function
    mov al, 10           ; Number of sectors
    mov ch, 0           ; Cylinder 0
    mov dh, 0           ; Head 0
    mov cl, 2           ; Sector 2
    mov dl, [BOOT_DRIVE] ; Use saved drive ID
    int 0x13
    jc disk_error

    jmp 0x0000:0x8000

disk_error:
    mov ah, 0x0E
    mov al, 'E'     ; 'E' for Error
    int 0x10
    hlt

BOOT_DRIVE: db 0

times 510 - ($ - $$) db 0
dw 0xAA55
