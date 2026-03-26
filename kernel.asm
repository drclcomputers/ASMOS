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
    	mov cl, 0xFF
     	call fill_screen

      	mov ax, 0          ; Y position
		mov bx, 0          ; X position
		mov si, hello_text     ; String pointer
		mov cl, 10         ; Color
		call draw_string

    	mov ax, 30          ; Y position
      	mov bx, 50          ; X position
    	mov dx, 200         ; Width
    	mov si, 150         ; Height
    	mov cl, 0x08        ; Dark gray content color
    	call draw_window

        call update_mouse
        call render_cursor

        call blit

        jmp .loop

    jmp $

hello_text: db "Hello World!", 0
message: db "Text is working!", 0

%include "utility/utility.asm"
%include "graphics/graphics.asm"
%include "io/mouse.asm"
%include "fonts/bitmap.asm"

times 5120 - ($ - $$) db 0
