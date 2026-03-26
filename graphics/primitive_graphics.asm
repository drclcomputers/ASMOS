drawdot: ; ax = y, bx = x, cl = color
    push ax
    push di
    push dx

    shl ax, 8        ; ax = y * 256
    mov dx, ax       ; store (y * 256) in dx

    pop ax           ; get original Y back
    shl ax, 6        ; ax = y * 64

    add ax, dx       ; ax = (y * 256) + (y * 64) = y * 320
    add ax, bx       ; add x-coordinate

    mov di, ax
    mov [es:di], cl

    pop dx
    pop di
    ret

drawline: ; ax=y, bx=x, dx=len, cl=color, si=xstep, bp=ystep (x=1, y=1 -> \ jos; x=1, y=-1 -> / sus; x=-1, y=1 -> / jos; x=-1, y=-1 -> \ sus)
    .loop:
        push ax
        push bx
        push dx

        call drawdot

        pop dx
        pop bx
        pop ax

        add bx, si    ; Move X by the step (0, 1, or even -1)
        add ax, bp    ; Move Y by the step (0, 1, or even -1)

        dec dx
        jnz .loop
        ret

drawrect: ; ax - Y, bx - X, dx - Width, si - Height, cl - Color
    push ax
    push bx
    push dx
    push si

    .row_loop:
        push dx         ; Save width (length of line)
        push bx         ; Save starting X

        ; Setup for drawline:
        ; ax = current y (already set)
        ; bx = current x (already set)
        ; dx = width (already pushed)
        ; cl = color (already set)
        push si
        mov si, 1       ; Step X = 1 (horizontal)
        mov bp, 0       ; Step Y = 0
        call drawline
        pop si

        pop bx          ; Restore X
        pop dx          ; Restore width

        inc ax          ; Move to next Y row
        dec si          ; Decrement height counter
        jnz .row_loop

        pop si
        pop dx
        pop bx
        pop ax
        ret

; Fast drawrect using rep stosb — no drawline, no drawdot
; ax=Y, bx=X, dx=Width, si=Height, cl=Color
drawrect_fast:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    ; Compute starting offset: Y*320 + X → DI
    push ax
    shl ax, 8           ; ax = Y * 256
    mov di, ax
    pop ax
    shl ax, 6           ; ax = Y * 64
    add di, ax          ; di = Y * 320
    add di, bx          ; di = Y * 320 + X

    mov al, cl          ; color byte to fill

.row:
    push di             ; save row start
    mov cx, dx          ; cx = width
    rep stosb           ; fill entire row in one shot
    pop di              ; restore row start
    add di, 320         ; move to next row (320 bytes per row)
    dec si
    jnz .row

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret
