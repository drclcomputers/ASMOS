; ax - Y, bx - X, dx - Width, si - Height, cl - Color, di - Title string ptr
draw_window:
    push ax
    push bx
    push dx
    push si
    push cx

    mov [window_y], ax
    mov [window_x], bx
    mov [window_width], dx
    mov [window_height], si
    mov [window_color], cl
    mov [window_title], di

    ; Title bar - ax = Y, bx = X, dx = Width, si = Height, cl = Color
    mov si, 10
    mov cl, 51
    call drawrect_fast

    ; Close button - ax = Y, bx = X, dx = Width, si = Height, cl = Color
   	add ax, 2
    add bx, 2
    mov si, 7
    push dx
    mov dx, 7
    mov cl, 0x10
    call drawrect_fast
    pop dx
    sub ax, 2
    sub bx, 2

    ; Title text — X+12 to clear the close button, Y+2 to center in bar
    mov si, [window_title]
    mov bx, [window_x]
    add bx, 12
    mov ax, [window_y]
    add ax, 2
    mov cl, 0
    mov ch, 1
    call draw_string

    ; Content area
    mov ax, [window_y]
    add ax, 10
    mov bx, [window_x]
    mov dx, [window_width]
    mov si, [window_height]
    sub si, 10
    mov cl, [window_color]
    call drawrect_fast

    ; Inner content rect
    mov ax, [window_y]
    add ax, 18
    mov bx, [window_x]
    add bx, 8
    mov dx, [window_width]
    sub dx, 16
    mov si, [window_height]
    sub si, 26
    mov cl, 0xF0
    call drawrect_fast

    pop cx
    pop si
    pop dx
    pop bx
    pop ax
    ret

; Window data storage
window_x dw 0
window_y dw 0
window_width dw 0
window_height dw 0
window_color db 0
window_title dw 0

init_grayscale_palette:
    ; Port 0x3C8 = palette index write
    ; Port 0x3C9 = palette data (R, G, B in sequence)

    mov dx, 0x3C8
    xor al, al
    out dx, al

    inc dx

    xor cx, cx
.gray_loop:
    mov al, cl          ; Use counter value as gray level

    out dx, al          ; Red
    out dx, al          ; Green
    out dx, al          ; Blue

    inc cx
    cmp cx, 256
    jne .gray_loop

    ret


%include "graphics/primitive_graphics.asm"
