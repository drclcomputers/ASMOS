BITS 16
ORG 0x8000

kernel_main:
    ; Set Video Mode 13h
    mov ax, 0x0013
    int 0x10

    xor ax, ax
    mov ds, ax

    mov ax, 0x1000
    mov es, ax

    call init_mouse
    call init_grayscale_palette

    .loop:
        ; Clear buffer
        mov cl, 0xFF
        call fill_screen

        ; ── Draw UI components ─────────────────────────
        mov si, my_menus
        mov cx, 3
        call draw_menubar

        mov si, my_buttons
        mov cx, NUM_BUTTONS
        call draw_buttons

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

        ; ── Handle mouse & Blit ────────────────────────
        call update_mouse

        ; Simple click handler
        test byte [mouse_byte1], 0x01
        jz .no_click

        mov si, my_buttons
        mov cx, NUM_BUTTONS
        call hit_test_buttons
        test al, al
        jz .no_click

        cmp al, BTN_OK
        je .clicked_ok
        cmp al, BTN_CANCEL
        je .clicked_cancel
        jmp .no_click

    .clicked_ok:
        mov word [status_msg], 'OK'
        mov byte [status_msg+2], 0
        jmp .no_click

    .clicked_cancel:
        mov word [status_msg], 'No'
        mov byte [status_msg+2], 0

    .no_click:
        call render_cursor
        call blit
        jmp .loop

; ============================================================
; FIXED DATA TABLES
; ============================================================

BTN_OK      equ 1
BTN_CANCEL  equ 2
BTN_QUIT    equ 3
NUM_BUTTONS equ 3
NUM_LABELS  equ 2

; Button table (12 bytes/rec): x(dw), y(dw), w(dw), h(dw), label(dw), color(db), id(db)
my_buttons:
    dw 70,  160, 50, 12, btn_ok_lbl
    db 0xDD, BTN_OK

    dw 130, 160, 50, 12, btn_cancel_lbl
    db 0xDD, BTN_CANCEL

    dw 280, 185, 35, 10, btn_quit_lbl
    db 0xAA, BTN_QUIT

; Label table (8 bytes/rec): x(dw), y(dw), text_ptr(dw), color(db), font(db)
my_labels:
    dw 70,  50, lbl_hello
    db 0x00, 0

    dw 70,  62, status_msg ; Point directly to the string, not a pointer to it
    db 0x40, 1

; Menu Item (5 bytes/rec): label(dw), id(db), enabled(db), separator(db)
file_items:
    dw fi_new
    db 1, 1, 0
    dw fi_open
    db 2, 1, 0
    dw fi_save
    db 3, 1, 1
    dw fi_quit
    db 4, 1, 0

edit_items:
    dw ei_cut
    db 10, 0, 0
    dw ei_copy
    db 11, 0, 0
    dw ei_paste
    db 12, 0, 0

view_items:
    dw vi_icons
    db 20, 1, 0
    dw vi_list
    db 21, 1, 0

my_menus:
    dw mn_file, file_items
    db 4, 0
    dw 0
    dw mn_edit, edit_items
    db 3, 0
    dw 0
;
btn_ok_lbl:     db "OK", 0
btn_cancel_lbl: db "Cancel", 0
btn_quit_lbl:   db "Quit", 0
lbl_hello:      db "Hello, World!", 0
mn_file:        db "File", 0
mn_edit:        db "Edit", 0
mn_view:        db "View", 0
fi_new:         db "New", 0
fi_open:        db "Open...", 0
fi_save:        db "Save", 0
fi_quit:        db "Quit", 0
ei_cut:         db "Cut", 0
ei_copy:        db "Copy", 0
ei_paste:       db "Paste", 0
vi_icons:       db "as Icons", 0
vi_list:        db "as List", 0
win_title:      db "Untitled", 0
status_msg:     db "Ready.             ", 0

%include "utility/utility.asm"
%include "graphics/graphics.asm"
%include "io/mouse.asm"
%include "fonts/bitmap.asm"
%include "ui/ui.asm"

times 20480 - ($ - $$) db 0
