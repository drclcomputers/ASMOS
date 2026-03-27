BITS 16
ORG 0x8000

kernel_main:
    mov ax, 0x0013
    int 0x10
    xor ax, ax
    mov ds, ax
    mov ax, 0x1000
    mov es, ax

    call init_mouse
    call init_grayscale_palette

    .loop:
        mov cl, 0xCC
        call fill_screen

        ; ── Draw menu bar ──────────────────────────────
        mov si, my_menus
        mov cx, 3               ; 3 menus: File, Edit, View
        call draw_menubar

        ; ── Draw all buttons in one call ───────────────
        mov si, my_buttons
        mov cx, NUM_BUTTONS
        call draw_buttons

        ; ── Draw all labels in one call ────────────────
        mov si, my_labels
        mov cx, NUM_LABELS
        call draw_labels

        ; ── Draw a window ──────────────────────────────
        mov ax, 40
        mov bx, 60
        mov dx, 180
        mov si, 120
        mov cl, 0xEE
        mov di, win_title
        call draw_window

        ; ── Handle mouse ───────────────────────────────
        call update_mouse

        ; Check button clicks (only on left press, not hold)
        test byte [mouse_byte1], 0x01
        jz .no_click

        mov si, my_buttons
        mov cx, NUM_BUTTONS
        call hit_test_buttons   ; returns ID in AL
        test al, al
        jz .no_click

        ; AL = button ID that was clicked
        cmp al, BTN_OK
        je .clicked_ok
        cmp al, BTN_CANCEL
        je .clicked_cancel
        cmp al, BTN_QUIT
        je .clicked_quit
        jmp .no_click

    .clicked_ok:
        mov byte [status_msg], 'O'
        mov byte [status_msg+1], 'K'
        mov byte [status_msg+2], '!'
        mov byte [status_msg+3], 0
        jmp .no_click

    .clicked_cancel:
        mov byte [status_msg], 'N'
        mov byte [status_msg+1], 'o'
        mov byte [status_msg+2], 0
        jmp .no_click

    .clicked_quit:
        cli
        hlt

    .no_click:
        call render_cursor
        call blit
        jmp .loop

    jmp $

; ============================================================
; UI DATA — define your entire interface here as tables.
; To add a new button: add one record below. No new code.
; ============================================================

; Button IDs
BTN_OK     equ 1
BTN_CANCEL equ 2
BTN_QUIT   equ 3
NUM_BUTTONS equ 3

; Button table: x, y, width, height, label ptr, bg_color, id
my_buttons:
    dw 70,  160, 50, 12    ; X, Y, W, H
    dw btn_ok_lbl          ; label
    db 0xDD, BTN_OK        ; color, id

    dw 130, 160, 50, 12
    dw btn_cancel_lbl
    db 0xDD, BTN_CANCEL

    dw 280, 185, 35, 10
    dw btn_quit_lbl
    db 0xAA, BTN_QUIT

; Label IDs
NUM_LABELS equ 2

; Label table: x, y, text ptr, color, font
my_labels:
    dw 70,  50
    dw lbl_hello
    db 0x00, 0             ; color=black, font=large

    dw 70,  62
    dw lbl_status_ptr      ; pointer to status_msg (dynamic)
    db 0x40, 1             ; color=dark, font=small

; Menu data
; Menu item records: label ptr, id, enabled, separator
file_items:
    dw fi_new,   1, 1, 0   ; New,  id=1, enabled, no separator
    dw fi_open,  2, 1, 0   ; Open, id=2, enabled
    dw fi_save,  3, 1, 1   ; Save, id=3, enabled, separator above
    dw fi_quit,  4, 1, 0   ; Quit, id=4, enabled

edit_items:
    dw ei_cut,   10, 0, 0  ; Cut,  id=10, disabled
    dw ei_copy,  11, 0, 0  ; Copy, id=11, disabled
    dw ei_paste, 12, 0, 0  ; Paste,id=12, disabled

view_items:
    dw vi_icons, 20, 1, 0
    dw vi_list,  21, 1, 0

; Menu table: label ptr, items ptr, item_count, x (filled by draw_menubar), reserved
my_menus:
    dw mn_file, file_items, 4, 0, 0
    dw mn_edit, edit_items, 3, 0, 0
    dw mn_view, view_items, 2, 0, 0

; ── Strings ──────────────────────────────────────────────
btn_ok_lbl:     db "OK", 0
btn_cancel_lbl: db "Cancel", 0
btn_quit_lbl:   db "Quit", 0

lbl_hello:      db "Hello, World!", 0
lbl_status_ptr: dw status_msg      ; pointer to dynamic string

mn_file:    db "File", 0
mn_edit:    db "Edit", 0
mn_view:    db "View", 0

fi_new:     db "New", 0
fi_open:    db "Open...", 0
fi_save:    db "Save", 0
fi_quit:    db "Quit", 0

ei_cut:     db "Cut", 0
ei_copy:    db "Copy", 0
ei_paste:   db "Paste", 0

vi_icons:   db "as Icons", 0
vi_list:    db "as List", 0

win_title:  db "Untitled", 0

status_msg: db "Ready.", 0

; ── Includes ─────────────────────────────────────────────
%include "utility/utility.asm"
%include "graphics/graphics.asm"
%include "io/mouse.asm"
%include "fonts/bitmap.asm"
%include "ui/ui.asm"

times 10240 - ($ - $$) db 0
