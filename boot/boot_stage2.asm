[bits 16]
[org 0x7E00]

KERNEL_ENTRY    equ 0x10000

%macro io_delay 0
    call _io_delay_impl
%endmacro

entry:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x6000
    sti

; ── Memory Detection (INT 15h, AH=88h) ──────────────────────────────────────
; AX = KB of extended memory above 1MB.  Result stored at 0x0500 as a
; uint32_t total-RAM byte count.  0 means the call failed or returned 0 KB;
; the kernel will use HEAP_END_MAX as a safe fallback in that case.

    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  dword [0x0500], 0      ; default: unknown

    mov  ah, 0x88
    int  0x15
    jc   .mem_store             ; CF set — call not supported; leave 0x0500 = 0

    test ax, ax
    jz   .mem_store             ; AX=0 — no extended memory reported

    ; AX = KB above 1MB (16-bit, up to 65535 KB ~ 64 MB)
    movzx eax, ax
    shl  eax, 10                ; eax = AX * 1024 (bytes above 1MB)
    add  eax, 0x100000          ; + 1MB base = total RAM in bytes

.mem_store:
    xor  bx, bx
    mov  ds, bx
    mov  dword [0x0500], eax    ; store result (0 if call failed)
    xor  ax, ax
    mov  ds, ax
    mov  es, ax

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

    jmp  .video_setup_done

; ── Mode X: 640×400×16 colour (EGA planar) ───────────────────────────────────
; Strategy: use BIOS mode 0x12 (640×480×16c) as baseline — it correctly sets
; up the sequencer, GC, AC, and palette for 16-colour planar operation.
; We then only patch the CRTC vertical registers to get 400 lines @ 70 Hz
; instead of 480 lines @ 60 Hz, and fix the Misc Output sync polarity.
;
; Memory layout per plane:
;   640 pixels / 8 bits per byte = 80 bytes per scanline per plane
;   80 bytes × 400 lines = 32 000 bytes per plane  (4 planes × 32 KB = 128 KB total)
; CRTC offset register (0x13) counts in 16-bit word units:
;   80 bytes / 2 = 40 = 0x28

.setup_mode_x:
; Step 1: set BIOS mode 0x12 — 640×480×16 colour planar
;   The BIOS correctly programs: SEQ (unchained, odd/even off, all planes),
;   GC (write mode 0, no shift256, no chain), AC (EGA palette, pan=0),
;   and loads the default 16-colour EGA/VGA palette into the DAC.
    mov  ax, 0x0012
    int  0x10

; Step 2: unlock CRTC registers 0–7 (bit 7 of reg 0x11 = write protect)
    mov  dx, 0x03D4
    mov  al, 0x11
    out  dx, al
    inc  dx
    in   al, dx
    and  al, 0x7F
    out  dx, al
    dec  dx

; Step 3: Misc Output — switch sync polarity for 640×400 @ 70 Hz
;   0x63 = 0b_0110_0011
;     bit7=0  negative VSYNC  (monitors use HSYNC-/VSYNC+ to identify 70 Hz 400-line)
;     bit6=1  negative HSYNC
;     bit3:2=00  25.175 MHz dot clock
;     bit1=1  RAM enabled
;     bit0=1  I/O to colour address (3Dx)
;   IMPORTANT: write Misc Output BEFORE touching CRTC timing regs so the
;   dot clock change takes effect before we reprogram the counters.
    mov  dx, 0x03C2
    mov  al, 0xA3
    out  dx, al

; Step 4: reprogram only the CRTC vertical registers for 400 lines.
;   Horizontal regs are unchanged from mode 0x12 (already correct for 640px).
;   All values below are for 640×400 @ 70 Hz with a 25.175 MHz dot clock.
;
;   Scanline counts (0-based):
;     Active lines    : 0 – 399      (400 lines)
;     VBlank start    : 400  = 0x190 → low byte 0x90
;     VRetrace start  : 412  = 0x19C → low byte 0x9C
;     VRetrace end    : 414          → 0x8E (bits[3:0]=end, bit7=protect off)
;     VBlank end      : 447          → 0x8F in reg 0x16
;     VTotal          : 448  = 0x1C0 → low byte 0xC0  (one more than last line)
;
;   Overflow register 0x07 encodes the high bits of five counters:
;     VTotal    bit8 → bit0,  bit9 → bit5
;     VDisplay  bit8 → bit1,  bit9 → bit6
;     VRetrace  bit8 → bit2,  bit9 → bit7
;     VBlank    bit8 → bit3
;     LineCmp   bit8 → bit4
;   For 400-line 70 Hz:
;     VTotal   =0x1C0: bit8=1→b0, bit9=0→b5  =  0b_0000_0001
;     VDisplay =0x18F: bit8=1→b1, bit9=0→b6  =  0b_0000_0010
;     VRetrace =0x19C: bit8=1→b2, bit9=0→b7  =  0b_0000_0100
;     VBlank   =0x190: bit8=1→b3             =  0b_0000_1000
;     LineCmp  =0x3FF: bit8=1→b4             =  0b_0001_0000
;   Sum = 0b_0001_1111 = 0x1F

    mov  dx, 0x03D4

    ; VTotal low byte  (reg 0x06)
    mov  al, 0x06
    out  dx, al
    inc  dx
    mov  al, 0xC0
    out  dx, al
    dec  dx

    ; Overflow          (reg 0x07)
    mov  al, 0x07
    out  dx, al
    inc  dx
    mov  al, 0x1F
    out  dx, al
    dec  dx

    ; Max Scan Line – no double-scan, line-compare bits[4:0]=0  (reg 0x09)
    mov  al, 0x09
    out  dx, al
    inc  dx
    mov  al, 0x00
    out  dx, al
    dec  dx

    ; VDisplay End low byte  (reg 0x12)  400-1 = 399 = 0x18F → 0x8F
    mov  al, 0x12
    out  dx, al
    inc  dx
    mov  al, 0x8F
    out  dx, al
    dec  dx

    ; Offset / logical width  (reg 0x13)
    ; 640px / 8 bits = 80 bytes/line per plane → 80/2 = 40 words = 0x28
    mov  al, 0x13
    out  dx, al
    inc  dx
    mov  al, 0x28
    out  dx, al
    dec  dx

    ; VBlank Start low byte  (reg 0x15)  0x190 → 0x90
    mov  al, 0x15
    out  dx, al
    inc  dx
    mov  al, 0x90
    out  dx, al
    dec  dx

    ; VBlank End             (reg 0x16)
    mov  al, 0x16
    out  dx, al
    inc  dx
    mov  al, 0xB5
    out  dx, al
    dec  dx

    ; VRetrace Start low byte (reg 0x10)  0x19C → 0x9C
    mov  al, 0x10
    out  dx, al
    inc  dx
    mov  al, 0x9C
    out  dx, al
    dec  dx

    ; VRetrace End            (reg 0x11)  protect=0, value = 0x8E
    mov  al, 0x11
    out  dx, al
    inc  dx
    mov  al, 0x8E
    out  dx, al
    dec  dx

; Step 5: clear all 4 planes
;   Enable all planes via Map Mask, then zero 80×400 = 32 000 bytes = 16 000 words
    mov  dx, 0x03C4
    mov  al, 0x02           ; Map Mask
    out  dx, al
    inc  dx
    mov  al, 0x0F           ; all 4 planes
    out  dx, al

    mov  ax, 0xA000
    mov  es, ax
    xor  di, di
    xor  ax, ax
    mov  cx, 0x7D00         ; 32 000 words = 64 000 bytes (clears all 4 planes at once)
    rep  stosw

; Store mode info
    xor  ax, ax
    mov  ds, ax
    mov  byte [0x0602], 0x01        ; mode = Mode X (EGA planar 16c)
    mov  word [0x0604], 640         ; width
    mov  word [0x0606], 400         ; height

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
    cli
    lgdt [gdt_descriptor]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    db  0x66, 0xEA          ; far jump 32-bit, encoding manual complet
    dd  0x00010000          ; sare direct la loader
    dw  0x0008


; ── Subroutines ──────────────────────────────────────────────────────────────
_io_delay_impl:
    push ax
    xor  ax, ax
    out  0x80, al
    pop  ax
    ret

kbc_wait_write:
    push ax
    push cx
    mov  cx, 0x8000
.kw_loop:
    io_delay
    in   al, 0x64
    test al, 0x02
    jz   .kw_done
    loop .kw_loop
.kw_done:
    pop  cx
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

; ── GDT ──────────────────────────────────────────────────────────────────────
align 4
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
