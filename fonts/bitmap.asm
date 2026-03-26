; Draws Char
; Requires char_x, char_y, char_color to be set before calling.
; al = ASCII character to draw.
draw_char:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    sub al, 32
    jb .done
    cmp al, 95
    jae .done

    xor ah, ah
    shl ax, 3              ; ax = index * 8
    mov di, font1_data     ; Get base address
    add di, ax             ; di = exact start of this character's 8 bytes

    xor si, si             ; si = row counter (0..7)

.row_loop:
    mov dl, [di]           ; dl = current row byte
    mov cx, 0              ; cx = column counter (0..7)

.bit_loop:
    test dl, 0x80
    jz .skip_pixel

    push dx                ; Protect current row pattern (dl)[cite: 3]
    push cx                ; Protect column counter (cx)[cite: 3]
    push di                ; Protect font pointer[cite: 3]

    mov ax, [char_y]
    add ax, si
    mov bx, [char_x]
    add bx, cx

    cmp ax, 199
    jg .pop_out
    cmp bx, 319
    jg .pop_out

    mov cl, [char_color]
    call drawdot

.pop_out:
    pop di
    pop cx
    pop dx

.skip_pixel:
    shl dl, 1
    inc cx
    cmp cx, 8
    jl .bit_loop

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; draw_string: si=string ptr, bx=X, ax=Y, cl=color
draw_string:
    mov [char_color], cl
    mov [char_x], bx
    mov [char_y], ax

    push ax
    push bx
    push cx
    push si

.loop:
    mov al, [si]
    test al, al
    jz .done

    call draw_char

    mov bx, [char_x]
    add bx, 8
    mov [char_x], bx

    inc si
    jmp .loop

.done:
    pop si
    pop cx
    pop bx
    pop ax
    ret

; draw_string_wrapped: si=string ptr, bx=X, ax=Y, cl=color, dx=wrap width
draw_string_wrapped:
    mov [char_color], cl
    mov [wrap_width], dx
    mov [char_x], bx
    mov [char_y], ax

    push ax
    push bx
    push cx
    push si

.loop:
    mov al, [si]
    test al, al
    jz .done

    cmp al, 0x0A
    je .newline

    mov bx, [char_x]
    add bx, 8
    cmp bx, [wrap_width]
    jg .newline

    call draw_char

    mov bx, [char_x]
    add bx, 8
    mov [char_x], bx
    inc si
    jmp .loop

.newline:
    mov ax, [char_y]
    add ax, 10
    mov [char_y], ax
    mov word [char_x], 0
    inc si
    jmp .loop

.done:
    pop si
    pop cx
    pop bx
    pop ax
    ret

char_color  db 0
char_x      dw 0
char_y      dw 0
wrap_width  dw 0

%include "fonts/font1.asm"
