sleep: ; cx:dx - ms to sleep
    mov ah, 0x86
    int 0x15
    ret

clearscreen:
    xor di, di
    xor ax, ax
    mov cx, 64000
    cld
    rep stosb
    ret

blit:
    push ds
    push es
    push si
    push di
    push cx

    mov ax, 0xA000
    mov es, ax
    mov ax, 0x1000
    mov ds, ax
    xor si, si
    xor di, di
    mov cx, 32000
    cld
    rep movsw

    pop cx
    pop di
    pop si
    pop es
    pop ds
    ret

fill_screen:  ; cl = color
    xor di, di
    mov al, cl
    mov ah, cl
    mov cx, 32000
    cld
    rep stosw
    ret
