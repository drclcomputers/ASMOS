[bits 16]
[org 0x7E00]

VESA_MODE    equ 0x0101
KERNEL_ENTRY equ 0x8000

; COM1 serial debug macro — prints a single character
; Destroys: AX, DX
%macro serial_char 1
    mov al, %1
    mov dx, 0x3F8
%%wait:
    add dx, 5           ; 0x3FD = Line Status Register
    in  al, dx
    sub dx, 5
    test al, 0x20       ; TX empty?
    jz  %%wait
    mov al, %1
    out dx, al
%endmacro

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000

    ; Init COM1: 9600 baud, 8N1
    mov dx, 0x3F8
    add dx, 1       ; IER
    mov al, 0x00
    out dx, al
    sub dx, 1
    add dx, 3       ; LCR
    mov al, 0x80    ; DLAB on
    out dx, al
    sub dx, 3
    mov al, 0x0C    ; divisor low  (9600 baud from 1.8432 MHz)
    out dx, al
    add dx, 1
    mov al, 0x00    ; divisor high
    out dx, al
    sub dx, 1
    add dx, 3
    mov al, 0x03    ; 8N1, DLAB off
    out dx, al
    sub dx, 3

    sti

    serial_char 'S'     ; Stage2 start
    serial_char '2'
    serial_char 13
    serial_char 10

; ── E820 ─────────────────────────────────────────────────────────────────────
    mov word [0x0500], 0
    xor ax, ax
    mov es, ax
    mov di, 0x0504
    xor ebx, ebx
    mov edx, 0x534D4150
    xor bp, bp
.e820:
    mov eax, 0xE820
    mov ecx, 20
    int 0x15
    jc  .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    test ecx, ecx
    jz  .e820_next
    inc bp
    add di, 20
    cmp bp, 50
    jae .e820_done
.e820_next:
    test ebx, ebx
    jnz .e820
.e820_done:
    mov [0x0500], bp
    xor ax, ax
    mov ds, ax
    mov es, ax

    serial_char 'E'     ; E820 done

; ── VBE 4F01 ─────────────────────────────────────────────────────────────────
    xor ax, ax
    mov es, ax
    mov di, 0x0700
    mov ax, 0x4F01
    mov cx, VESA_MODE
    int 0x10
    xor bx, bx
    mov ds, bx
    mov es, bx

    mov dword [0x0600], 0

    cmp ax, 0x004F
    jne .vesa_banked

    mov al, [0x0700]
    test al, 0x01
    jz  .vesa_banked
    test al, 0x80
    jz  .vesa_banked

    mov eax, [0x0700 + 40]
    mov [0x0600], eax

    serial_char 'L'     ; LFB address found

    mov ax, 0x4F02
    mov bx, 0x4000 | VESA_MODE
    int 0x10
    xor bx, bx
    mov ds, bx
    mov es, bx
    cmp ax, 0x004F
    je  .vesa_done

.vesa_banked:
    serial_char 'B'     ; banked fallback
    mov dword [0x0600], 0
    mov ax, 0x4F02
    mov bx, VESA_MODE
    int 0x10
    xor bx, bx
    mov ds, bx
    mov es, bx

.vesa_done:
    serial_char 'V'     ; VESA done

; ── Protected mode ────────────────────────────────────────────────────────────
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; Serial 'K' — about to jump to kernel
    mov dx, 0x3FD
.w: in  al, dx
    test al, 0x20
    jz  .w
    mov dx, 0x3F8
    mov al, 'K'
    out dx, al

    jmp KERNEL_ENTRY

[bits 16]
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
