; Unified font renderer
; ch = 0  ->  font1 (8x8)
; ch = 1  ->  font2 (4x6, small)
;
; draw_string:         si=string ptr, bx=X, ax=Y, cl=color, ch=font
; draw_string_wrapped: si=string ptr, bx=X, ax=Y, cl=color, ch=font, dx=wrap width

; Internal: draw_char reads character from [char_char], position from [char_x]/[char_y]
draw_char:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov al, [char_char]
    sub al, 32
    jb .done
    cmp al, 95
    jae .done

    cmp byte [char_font], 1
    je .use_small

; --- Font1: 8x8 ---
.use_large:
    xor ah, ah
    shl ax, 3               ; ax = index * 8
    mov di, font1_data
    add di, ax              ; di = pointer to char data

    xor si, si              ; si = row (0..7)

.large_row_loop:
    mov dl, [di]
    mov cx, 0               ; cx = col (0..7)

.large_bit_loop:
    test dl, 0x80
    jz .large_skip

    push dx
    push cx
    push di

    mov ax, [char_y]
    add ax, si
    mov bx, [char_x]
    add bx, cx

    cmp ax, 199
    jg .large_pop
    cmp bx, 319
    jg .large_pop

    mov cl, [char_color]
    call drawdot

.large_pop:
    pop di
    pop cx
    pop dx

.large_skip:
    shl dl, 1
    inc cx
    cmp cx, 8
    jl .large_bit_loop

    inc di
    inc si
    cmp si, 8
    jl .large_row_loop

    jmp .done

; --- Font2: 4x6 ---
.use_small:
    xor ah, ah
    mov di, ax
    shl di, 1               ; di = index * 2
    add di, ax              ; di = index * 3
    shl di, 1               ; di = index * 6
    mov si, font2_data
    add si, di              ; si = pointer to char data

    xor di, di              ; di = row (0..5)

.small_row_loop:
    mov dl, [si]
    shl dl, 4               ; shift low nibble to high for MSB testing
    mov cx, 0               ; cx = col (0..3)

.small_bit_loop:
    test dl, 0x80
    jz .small_skip

    push dx
    push cx
    push si

    mov ax, [char_y]
    add ax, di
    mov bx, [char_x]
    add bx, cx

    cmp ax, 199
    jg .small_pop
    cmp bx, 319
    jg .small_pop

    mov cl, [char_color]
    call drawdot

.small_pop:
    pop si
    pop cx
    pop dx

.small_skip:
    shl dl, 1
    inc cx
    cmp cx, 4
    jl .small_bit_loop

    inc si
    inc di
    cmp di, 6
    jl .small_row_loop

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret


; draw_string: si=string ptr, bx=X, ax=Y, cl=color, ch=font
draw_string:
    mov [char_color], cl
    mov [char_font], ch
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

    mov [char_char], al         ; store char for draw_char to read
    call draw_char

    ; advance X
    mov bx, [char_x]
    cmp byte [char_font], 1
    je .small_advance
    add bx, 8
    jmp .store_x
.small_advance:
    add bx, 5
.store_x:
    mov [char_x], bx

    inc si
    jmp .loop

.done:
    pop si
    pop cx
    pop bx
    pop ax
    ret


; draw_string_wrapped: si=string ptr, bx=X, ax=Y, cl=color, ch=font, dx=wrap width
draw_string_wrapped:
    mov [char_color], cl
    mov [char_font], ch
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

    ; check wrap
    mov bx, [char_x]
    cmp byte [char_font], 1
    je .small_check
    add bx, 8
    jmp .check_wrap
.small_check:
    add bx, 5
.check_wrap:
    cmp bx, [wrap_width]
    jg .newline

    mov [char_char], al
    call draw_char

    mov bx, [char_x]
    cmp byte [char_font], 1
    je .small_adv
    add bx, 8
    jmp .store_x
.small_adv:
    add bx, 5
.store_x:
    mov [char_x], bx
    inc si
    jmp .loop

.newline:
    mov ax, [char_y]
    cmp byte [char_font], 1
    je .small_nl
    add ax, 10
    jmp .store_y
.small_nl:
    add ax, 8
.store_y:
    mov [char_y], ax
    mov word [char_x], 0
    cmp al, 0x0A
    jne .loop
    inc si
    jmp .loop

.done:
    pop si
    pop cx
    pop bx
    pop ax
    ret


char_char   db 0
char_color  db 0
char_font   db 0
char_x      dw 0
char_y      dw 0
wrap_width  dw 0

%include "fonts/font1.asm"
%include "fonts/font2.asm"
