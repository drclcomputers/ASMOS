; BUTTON RECORD FORMAT (12 bytes each):
;   dw  x, y, width, height   ; position and size
;   dw  label                 ; pointer to null-terminated string
;   db  bg_color              ; background color (palette index)
;   db  id                    ; unique ID returned on click
;
; LABEL RECORD FORMAT (9 bytes each):
;   dw  x, y                  ; position
;   dw  text                  ; pointer to string
;   db  color                 ; text color
;   db  font                  ; 0=large, 1=small
;
; MENU ITEM RECORD FORMAT (5 bytes each):
;   dw  label                 ; pointer to string
;   db  id                    ; unique ID
;   db  enabled               ; 1=enabled, 0=grayed
;   db  separator             ; 1=draw a line above this item
;
; MENU RECORD FORMAT (8 bytes each):
;   dw  label                 ; pointer to menu title string
;   dw  items                 ; pointer to array of menu item records
;   db  item_count            ; number of items
;   db  x                     ; X position in menu bar (set by init_menubar)
;   dw  _reserved

UI_BUTTON_SIZE   equ 12
UI_LABEL_SIZE    equ 8
UI_MENUITEM_SIZE equ 5
UI_MENU_SIZE     equ 7

MENUBAR_HEIGHT   equ 12
MENUBAR_COLOR    equ 0xFF
MENUBAR_TEXT     equ 0x00

; Draw buttons
; si = pointer to button table
; cx = number of buttons
draw_buttons:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

.loop:
    push cx
    push si

    mov ax, [si+2]      ; Y
    mov bx, [si+0]      ; X
    mov dx, [si+4]      ; width
    mov di, [si+6]      ; height (reuse di temporarily)
    push di
    mov cl, [si+10]     ; bg_color
    pop di

    push si
    mov si, di          ; si = height
    call drawrect_fast
    pop si

    mov ax, [si+2]
    mov bx, [si+0]
    mov dx, [si+4]
    push si
    mov si, [si+6]      ; height
    mov cl, 0x00        ; black border
    call draw_rect_outline
    pop si

    ; Draw button centered
    mov di, [si+8]      ; label pointer
    test di, di
    jz .no_label

    mov bx, [si+0]
    add bx, 4
    mov ax, [si+2]
    add ax, 2
    mov [si_save], si
    mov si, di
    mov cl, 0x00
    mov ch, 1
    call draw_string
    mov si, [si_save]

.no_label:
    pop si
    pop cx

    add si, UI_BUTTON_SIZE
    dec cx
    jnz .loop

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Check if mouse click on button
; si = pointer to button table
; cx = number of buttons
; Returns: al = button ID if hit (1-255), or 0 if no hit
hit_test_buttons:
    push bx
    push cx
    push dx
    push si

    mov bx, [mouse_x]   ; current mouse X
    mov dx, [mouse_y]   ; current mouse Y

.loop:
    ; Check X bounds
    mov ax, [si+0]      ; btn.x
    cmp bx, ax
    jl .miss

    add ax, [si+4]      ; btn.x + width
    cmp bx, ax
    jge .miss

    ; Check Y bounds
    mov ax, [si+2]      ; btn.y
    cmp dx, ax
    jl .miss

    add ax, [si+6]      ; btn.y + height
    cmp dx, ax
    jge .miss

    mov al, [si+11]     ; return button ID
    jmp .done

.miss:
    add si, UI_BUTTON_SIZE
    dec cx
    jnz .loop

    xor al, al          ; no hit

.done:
    pop si
    pop dx
    pop cx
    pop bx
    ret

; Draw labels in table
; si = pointer to label table
; cx = number of labels
draw_labels:
    push ax
    push bx
    push cx
    push si

.loop:
    push cx
    push si

    mov bx, [si+0]      ; X
    mov ax, [si+2]      ; Y
    mov di, [si+4]      ; text pointer
    mov cl, [si+6]      ; color
    mov ch, [si+7]      ; font

    push si
    mov si, di
    call draw_string
    pop si

    pop si
    pop cx

    add si, UI_LABEL_SIZE
    dec cx
    jnz .loop

    pop si
    pop cx
    pop bx
    pop ax
    ret

; Draw menubar and menus
; si = pointer to menu table
; cx = number of menus
draw_menubar:
    push ds
    push es
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    xor ax, ax
    mov ds, ax

    xor ax, ax          ; Y = 0
    xor bx, bx          ; X = 0
    mov dx, 320         ; Width
    mov [si_tmp], si
    mov si, MENUBAR_HEIGHT
    mov cl, MENUBAR_COLOR
    call drawrect_fast

    xor bx, bx
    mov ax, MENUBAR_HEIGHT
    mov dx, 320
    mov si, 1
    mov cl, 0x00        ; Black
    call drawrect_fast

    mov si, [si_tmp]    ; Restore menu table pointer
    mov bx, 6           ; Starting X for first menu

.menu_loop:
    push cx             ; number of menus left
    push si

    mov di, [si+0]

    mov ax, 2           ; Y = 2
    mov cl, MENUBAR_TEXT
    mov ch, 1

    push si
    mov si, di
    call draw_string
    pop si

    mov ax, bx
    mov [si+4], al      ; Store X start

    mov di, [si+0]
    call strlen_di      ; returns length in CX

    mov ax, cx          ; CX = string length
    mov dl, 5
    mul dl              ; AX = length * 5
    add ax, 10          ; padding
    add bx, ax

    pop si
    pop cx

    add si, UI_MENU_SIZE
    dec cx
    jnz .menu_loop

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    pop ds
    ret

; Returns index of menu title clicked, or 0xFF if none
; si = pointer to menu table
; cx = number of menus
hit_test_menubar:
    push bx
    push cx
    push dx
    push si

    mov ax, [mouse_y]
    cmp ax, MENUBAR_HEIGHT
    jge .no_hit

    mov bx, [mouse_x]
    xor dx, dx

.loop:
    xor ax, ax
    mov al, [si+4]
    cmp bx, ax
    jl .miss

    mov di, [si+0]
    call strlen_di
    push cx
    mov ax, cx
    mov cl, 5
    mul cl
    add ax, 8
    pop cx

    push dx
    xor dx, dx
    mov dl, [si+4]
    add ax, dx
    pop dx

    cmp bx, ax
    jge .miss

    mov al, dl
    jmp .done

.miss:
    inc dx
    add si, UI_MENU_SIZE
    dec cx
    jnz .loop

.no_hit:
    mov al, 0xFF

.done:
    pop si
    pop dx
    pop cx
    pop bx
    ret

; Draw a dropdown menu for a given menu record
; si = pointer to single menu record
; bx = X position to draw at
draw_dropdown:
    push ax
    push bx
    push cx
    push dx
    push si

    ; Count items and compute height
    mov cl, [si+4]
    xor ch, ch
    mov ax, cx
    mov dl, 9
    mul dl                  ; height = item_count * 9
    add ax, 4               ; +4 padding
    mov [dropdown_height], ax

    ; Draw background
    mov ax, MENUBAR_HEIGHT
    inc ax
    mov dx, 80
    mov [si_tmp2], si
    push ax
    mov si, [dropdown_height]
    mov cl, MENUBAR_COLOR
    call drawrect_fast
    pop ax

    ; Draw border
    mov ax, MENUBAR_HEIGHT
    inc ax
    mov dx, 80
    mov si, [dropdown_height]
    mov cl, 0x00
    call draw_rect_outline

    mov si, [si_tmp2]
    mov di, [si+2]
    mov cl, [si+4]
    xor ch, ch

    mov ax, MENUBAR_HEIGHT
    add ax, 4               ; first item Y

.item_loop:
    push cx
    push di
    push ax

    cmp byte [di+4], 1
    jne .no_sep
    push ax
    push bx
    mov dx, 78
    mov si, 1
    mov cl, 0xAA
    call drawrect_fast
    pop bx
    pop ax

.no_sep:
    mov cl, 0x00            ; black text = enabled
    cmp byte [di+3], 0
    jne .draw_item
    mov cl, 0x80            ; gray text = disabled

.draw_item:
    push bx
    add bx, 4               ; indent text
    push si
    mov si, [di+0]
    mov ch, 1
    call draw_string
    pop si
    pop bx

    pop ax
    pop di
    pop cx

    add di, UI_MENUITEM_SIZE
    add ax, 9               ; next item Y
    dec cx
    jnz .item_loop

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Returns item ID clicked in open dropdown, or 0 if none
; si = pointer to single menu record
; bx = X of dropdown
hit_test_dropdown:
    push bx
    push cx
    push si

    ; Check X bounds
    mov ax, [mouse_x]
    cmp ax, bx
    jl .no_hit
    add bx, 80
    cmp ax, bx
    jge .no_hit

    ; Check Y
    mov ax, [mouse_y]
    sub ax, MENUBAR_HEIGHT
    sub ax, 4
    js .no_hit

    ; row = (mouse_y - MENUBAR_HEIGHT - 4) / 9
    xor dx, dx
    mov cx, 9
    div cx

    ; Validate row index < item_count
    xor ch, ch
    mov cl, [si+4]
    cmp ax, cx
    jge .no_hit

    ; Get item at that index
    mov cx, ax
    mov si, [si+2]
    mov dx, UI_MENUITEM_SIZE
    mul dx                  ; offset = row * item_size
    add si, ax

    cmp byte [si+3], 0
    je .no_hit

    mov al, [si+2]          ; return item ID
    jmp .done

.no_hit:
    xor al, al

.done:
    pop si
    pop cx
    pop bx
    ret

; Draw border
; ax=Y, bx=X, dx=Width, si=Height, cl=Color
draw_rect_outline:
    push ax
    push bx
    push cx
    push dx
    push si

    ; Top edge
    push si
    mov si, 1
    call drawrect_fast
    pop si

    ; Bottom edge
    push ax
    push si
    add ax, si      ; Add height to Y
    dec ax
    mov si, 1
    call drawrect_fast
    pop si
    pop ax

    ; Left edge
    push dx
    mov dx, 1
    call drawrect_fast
    pop dx

    ; Right edge
    push bx
    push dx
    add bx, dx      ; Add width to X
    dec bx
    mov dx, 1
    call drawrect_fast
    pop dx
    pop bx

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; length of string at di, in cx.
strlen_di:
    push di
    xor cx, cx
.loop:
    cmp byte [di], 0
    je .done
    inc di
    inc cx
    jmp .loop
.done:
    pop di
    ret

si_save         dw 0
si_tmp          dw 0
si_tmp2         dw 0
dropdown_height dw 0
