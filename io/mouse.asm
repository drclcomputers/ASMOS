init_mouse:
    mov al, 0xA8        ; Enable mouse port
    out 0x64, al
    call wait_controller

    mov al, 0xD4        ; Send to mouse
    out 0x64, al
    call wait_controller
    mov al, 0xF4        ; Enable reporting
    out 0x60, al
    call wait_controller
    ret

wait_controller:
    mov cx, 0xFFFF
.loop:
    in al, 0x64
    test al, 0x02
    jz .done
    dec cx
    jnz .loop
.done:
    ret

update_mouse:
    in al, 0x64
    test al, 0x01
    jz .no_data

    in al, 0x60
    test al, 0x08       ; Check if packet start
    jz .no_data

    mov [mouse_byte1], al

    ; Get X
    mov cx, 0xFFFF
.wait_x:
    in al, 0x64
    test al, 0x01
    jnz .read_x
    dec cx
    jnz .wait_x
    jmp .no_data
.read_x:
    in al, 0x60
    mov [mouse_byte2], al

    ; Get Y
    mov cx, 0xFFFF
.wait_y:
    in al, 0x64
    test al, 0x01
    jnz .read_y
    dec cx
    jnz .wait_y
    jmp .no_data
.read_y:
    in al, 0x60
    mov [mouse_byte3], al

    ; Set mov
    movsx cx, byte [mouse_byte2]
    mov ax, [mouse_x]
    add ax, cx
    cmp ax, 0
    jge .x_pos
    mov ax, 0
.x_pos:
    cmp ax, 315
    jle .x_ok
    mov ax, 315
.x_ok:
    mov [mouse_x], ax

    movsx dx, byte [mouse_byte3]
    mov ax, [mouse_y]
    sub ax, dx
    cmp ax, 0
    jge .y_pos
    mov ax, 0
.y_pos:
    cmp ax, 195
    jle .y_ok
    mov ax, 195
.y_ok:
    mov [mouse_y], ax

.no_data:
    ret

render_cursor_simple:
    mov ax, [mouse_y]
    mov bx, [mouse_x]
    mov dx, 4
    mov si, 4
    mov cl, 0x0F
    call drawrect_fast
    ret

render_cursor:
    mov cl, 100                     ; default: white
    test byte [mouse_byte1], 0x01   ; left button pressed?
    jz .draw
    mov cl, 70                    ; red when held
.draw:
    mov ax, [mouse_y]
    mov bx, [mouse_x]
    mov dx, 4
    mov si, 4
    call drawrect_fast                   ; Fixed: was drawrect_fast
    ret

; Optional: Check button states (for future use)
check_left_button:
    test byte [mouse_byte1], 0x01   ; bit 0 = left button
    ret

check_right_button:
    test byte [mouse_byte1], 0x02   ; bit 1 = right button
    ret

check_middle_button:
    test byte [mouse_byte1], 0x04   ; bit 2 = middle button
    ret

mouse_x dw 160
mouse_y dw 100
mouse_byte1 db 0
mouse_byte2 db 0
mouse_byte3 db 0
