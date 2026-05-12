[bits 16]
[org 0x7E00]

KERNEL_ENTRY    equ 0x10000

%macro io_delay 0
    call _io_delay_impl
%endmacro

%macro screen_char 1
    pusha
    mov  ah, 0x0E
    mov  al, %1
    xor  bh, bh
    mov  bl, 0x07
    int  0x10
    popa
%endmacro

entry:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x6000
    sti

    screen_char 'S'
    screen_char '2'
    screen_char 13
    screen_char 10

; ── E820 ─────────────────────────────────────────────────────────────────────
    mov  word [0x0500], 0
    xor  ax, ax
    mov  es, ax
    mov  di, 0x0504
    xor  ebx, ebx
    mov  edx, 0x534D4150
    xor  bp, bp

.e820_loop:
    mov  eax, 0xE820
    mov  ecx, 20
    int  0x15
    jc   .e820_done
    cmp  eax, 0x534D4150
    jne  .e820_done
    test ecx, ecx
    jz   .e820_check_cont
    inc  bp
    add  di, 20
    cmp  bp, 50
    jae  .e820_done
.e820_check_cont:
    test ebx, ebx
    jnz  .e820_loop
.e820_done:
    mov  [0x0500], bp
    xor  ax, ax
    mov  ds, ax
    mov  es, ax

    screen_char 'E'

; ── Video Mode Setup ─────────────────────────────────────────────────────────
; Check flag at 0x0602 to determine which mode to use
; 0x00 = Mode 13h (320x200 chained linear)
; 0x01 = Mode X (640x480 unchained planar)
    xor  ax, ax
    mov  ds, ax
    mov  al, [0x0602]
    cmp  al, 0x01
    je   .setup_mode_x

; ── Mode 13h: 320×200×8bpp (chained linear) ──────────────────────────────────
.setup_mode_13h:
    mov  ax, 0x0013
    int  0x10

    ; Store mode info
    xor  ax, ax
    mov  ds, ax
    mov  byte [0x0602], 0x00        ; mode = Mode 13h
    mov  word [0x0604], 320         ; width
    mov  word [0x0606], 200         ; height

    ; Clear framebuffer (320x200 = 64000 bytes = 32000 words)
    xor  ax, ax
    mov  es, ax
    mov  di, 0xA000
    mov  es, di
    xor  di, di
    xor  ax, ax
    mov  cx, 0x7D00                 ; 32000 words (64000 bytes)
    rep  stosw

    screen_char 'M'                 ; Mode 13h active
    jmp  .video_setup_done

; ── Mode X: 640×480×8bpp (unchained planar) ──────────────────────────────────
.setup_mode_x:
; Step 1: set mode 13h baseline
    mov  ax, 0x0013
    int  0x10

; Step 2: unlock CRTC registers 0-7
    mov  dx, 0x03D4
    mov  al, 0x11
    out  dx, al
    inc  dx
    in   al, dx
    and  al, 0x7F
    out  dx, al
    dec  dx

; Step 3: write sequencer – unchain planes, disable chain-4
    mov  dx, 0x03C4

    mov  al, 0x04        ; Sequencer Memory Mode
    out  dx, al
    inc  dx
    in   al, dx
    and  al, 0xF7        ; clear chain-4 bit
    or   al, 0x06        ; extended memory, odd/even disable
    out  dx, al
    dec  dx

    mov  al, 0x02        ; Map Mask – enable all 4 planes
    out  dx, al
    inc  dx
    mov  al, 0x0F
    out  dx, al
    dec  dx

; Step 4: CRTC – reprogram for 640×400
; Note: 640×400 = 160 bytes/line × 400 lines = 64000 bytes (fits in 64KB VGA window)
    mov  dx, 0x03D4

    ; Horizontal Total
    mov  al, 0x00 & 0xFF
    out  dx, al
    inc  dx
    mov  al, 0x5F
    out  dx, al
    dec  dx

    ; Horizontal Display End
    mov  al, 0x01
    out  dx, al
    inc  dx
    mov  al, 0x4F
    out  dx, al
    dec  dx

    ; Horizontal Blank Start
    mov  al, 0x02
    out  dx, al
    inc  dx
    mov  al, 0x50
    out  dx, al
    dec  dx

    ; Horizontal Blank End
    mov  al, 0x03
    out  dx, al
    inc  dx
    mov  al, 0x82
    out  dx, al
    dec  dx

    ; Horizontal Retrace Start
    mov  al, 0x04
    out  dx, al
    inc  dx
    mov  al, 0x54
    out  dx, al
    dec  dx

    ; Horizontal Retrace End
    mov  al, 0x05
    out  dx, al
    inc  dx
    mov  al, 0x80
    out  dx, al
    dec  dx

    ; Vertical Total (419 lines - 1 = 0x1A2, use 0xA2 for low byte)
    mov  al, 0x06
    out  dx, al
    inc  dx
    mov  al, 0xA2
    out  dx, al
    dec  dx

    ; Overflow (VTotal bit 9 = 1, VDisplay bit 9 = 1, VRetrace bit 8 = 1)
    mov  al, 0x07
    out  dx, al
    inc  dx
    mov  al, 0x3D
    out  dx, al
    dec  dx

    ; Maximum Scan Line – disable double-scan (no double scan, line compare = 0)
    mov  al, 0x09
    out  dx, al
    inc  dx
    mov  al, 0x00
    out  dx, al
    dec  dx

    ; Vertical Retrace Start
    mov  al, 0x10
    out  dx, al
    inc  dx
    mov  al, 0xEA
    out  dx, al
    dec  dx

    ; Vertical Retrace End (write protect off, value = 0xEC)
    mov  al, 0x11
    out  dx, al
    inc  dx
    mov  al, 0xEC
    out  dx, al
    dec  dx

    ; Vertical Display End (400 - 1 = 399 = 0x18F)
    mov  al, 0x12
    out  dx, al
    inc  dx
    mov  al, 0x8F
    out  dx, al
    dec  dx

    ; Logical Width: 640/4 = 160 = 0xA0 (each plane holds 1/4 of pixels)
    mov  al, 0x13
    out  dx, al
    inc  dx
    mov  al, 0xA0
    out  dx, al
    dec  dx

    ; Vertical Blank Start
    mov  al, 0x15
    out  dx, al
    inc  dx
    mov  al, 0xE7
    out  dx, al
    dec  dx

    ; Vertical Blank End
    mov  al, 0x16
    out  dx, al
    inc  dx
    mov  al, 0x06
    out  dx, al
    dec  dx

; Step 5: Misc Output – select 25.175 MHz clock, enable colour
    mov  dx, 0x03C2
    mov  al, 0xE3
    out  dx, al

; Step 6: GC – disable chain, odd/even
    mov  dx, 0x03CE

    mov  al, 0x05        ; Graphics Mode
    out  dx, al
    inc  dx
    in   al, dx
    and  al, 0xEF        ; clear shift-256 bit
    out  dx, al
    dec  dx

    mov  al, 0x06        ; Miscellaneous
    out  dx, al
    inc  dx
    in   al, dx
    and  al, 0xFD        ; clear odd/even enable
    out  dx, al
    dec  dx

; Step 7: clear all 4 planes (0xA0000, 640x400 = 64000 bytes = 32000 words)
    xor  ax, ax
    mov  es, ax
    mov  di, 0xA000
    mov  es, di
    xor  di, di
    xor  ax, ax
    mov  cx, 0x7D00    ; 32000 words (64000 bytes)
    rep  stosw

; Store mode info
    xor  ax, ax
    mov  ds, ax
    mov  byte [0x0602], 0x01        ; mode = Mode X
    mov  word [0x0604], 640         ; width
    mov  word [0x0606], 400         ; height

    screen_char 'X'                 ; Mode X active

.video_setup_done:

; ── A20 ──────────────────────────────────────────────────────────────────────
    cli

    mov  ax, 0x2401
    int  0x15
    jnc  .a20_verify

.a20_kbc:
    call kbc_flush

    call kbc_wait_write
    mov  al, 0xAD
    out  0x64, al
    io_delay

    call kbc_wait_write
    mov  al, 0xD0
    out  0x64, al
    io_delay

    mov  cx, 0x4000
.obf_wait:
    io_delay
    in   al, 0x64
    test al, 0x01
    jnz  .obf_ok
    loop .obf_wait
    mov  al, 0xCF
    jmp  .kbc_write
.obf_ok:
    io_delay
    in   al, 0x60

.kbc_write:
    or   al, 0x02
    and  al, 0xFE
    mov  ah, al

    call kbc_wait_write
    mov  al, 0xD1
    out  0x64, al
    io_delay

    call kbc_wait_write
    mov  al, ah
    out  0x60, al
    io_delay

    call kbc_wait_write
    mov  al, 0xAE
    out  0x64, al
    io_delay
    call kbc_wait_write

    mov  cx, 0x800
.settle:
    io_delay
    loop .settle

    call kbc_flush

; ── A20 verify ───────────────────────────────────────────────────────────────
.a20_verify:
    xor  ax, ax
    mov  ds, ax
    mov  es, ax

    mov  eax, dword [ds:0x0600]
    push eax

    mov  dword [ds:0x0600], 0xCAFEBABE
    io_delay
    io_delay

    mov  ax, 0xFFFF
    mov  es, ax
    mov  eax, dword [es:0x0610]

    xor  bx, bx
    mov  ds, bx
    pop  ebx
    mov  dword [ds:0x0600], ebx

    xor  ax, ax
    mov  ds, ax
    mov  es, ax

    cmp  eax, 0xCAFEBABE
    jne  .a20_on

    in   al, 0x92
    test al, 0x02
    jnz  .a20_on
    or   al, 0x02
    and  al, 0xFE
    out  0x92, al
    io_delay
    io_delay
    io_delay

.a20_on:

; ── Protected mode ───────────────────────────────────────────────────────────
    cli
    call kbc_flush
    lgdt [gdt_descriptor]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:pm_entry

; ── Subroutines ──────────────────────────────────────────────────────────────

_io_delay_impl:
    push ax
    xor  ax, ax
    out  0x80, al
    pop  ax
    ret

kbc_wait_write:
    push ax
.kw_loop:
    io_delay
    in   al, 0x64
    test al, 0x02
    jnz  .kw_loop
    pop  ax
    ret

kbc_flush:
    push ax
    push cx
    mov  cx, 0x2000
.kf_loop:
    io_delay
    in   al, 0x64
    test al, 0x01
    jz   .kf_done
    io_delay
    in   al, 0x60
    loop .kf_loop
.kf_done:
    pop  cx
    pop  ax
    ret

; ── 32-bit protected mode entry ──────────────────────────────────────────────
[bits 32]
pm_entry:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x7C00
    jmp  KERNEL_ENTRY

; ── GDT ──────────────────────────────────────────────────────────────────────
[bits 16]
gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
gdt_data:
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

times (8 * 512) - ($ - $$) db 0
