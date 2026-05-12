[bits 16]
[org 0x7E00]

; ─────────────────────────────────────────────────────────────────────────────
; Stage 2 – real-hardware hardened (v3)
;
; Bug fixes vs v2:
;
;  1. GRAPHICS MODE TELETYPE BUG (the actual hang):
;     INT 10h/0Eh (teletype write) is undefined in VESA graphics modes on
;     real BIOSes. Calling screen_char *after* INT 10h/4F02h (mode-set) hangs
;     or corrupts state. Fix: print the status letter ('L','B','G') AND 'V'
;     BEFORE the mode-set call, never after. After .vesa_done we call no more
;     INT 10h functions at all.
;
;  2. E820 COUNT CORRUPTION:
;     A20 verify was writing a canary to 0x0500 which holds our E820 count.
;     Fix: use 0x0600 as the canary location (FB address word, which we
;     control). Its wrap-around alias is 0xFFFF:0x0610 = phys 0x100600,
;     wraps to 0x000600 with A20 off. Save/restore 0x0600 around the test.
;
;  3. io_delay INSIDE 'loop' CORRUPTS CX:
;     The old macro did push ax / out / pop ax inline. When expanded inside
;     a 'loop' body, the push/pop is fine — but it was NOT fine when called
;     from kbc_flush which used CX as a loop counter and called io_delay
;     (which never touched CX itself). The real corruption was that kbc_flush
;     did NOT save/restore CX. Fix: kbc_flush now push/pops CX explicitly,
;     and io_delay is a near-call (_io_delay_impl) to avoid macro bloat.
;
;  4. kbc_wait_write DID NOT SAVE AX:
;     On return, AL contained the last status byte read from port 0x64.
;     Callers that stored a value in AL/AX before calling kbc_wait_write
;     would have it silently clobbered. Fix: kbc_wait_write now saves/
;     restores AX.
; ─────────────────────────────────────────────────────────────────────────────

VESA_MODE       equ 0x0101
KERNEL_ENTRY    equ 0x10000
VBE_SCRATCH_SEG equ 0x2000          ; ModeInfoBlock scratch at linear 0x20000

; io_delay: one ISA bus cycle. Implemented as a subroutine call so it never
; expands inline and cannot interfere with loop counters or saved registers.
%macro io_delay 0
    call _io_delay_impl
%endmacro

; screen_char: BIOS teletype. ONLY safe in text/compatible mode.
; DO NOT call after any INT 10h graphics mode-set.
%macro screen_char 1
    pusha
    mov  ah, 0x0E
    mov  al, %1
    xor  bh, bh
    mov  bl, 0x07
    int  0x10
    popa
%endmacro

; ─────────────────────────────────────────────────────────────────────────────
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

; ── VBE mode select ───────────────────────────────────────────────────────────
; KEY RULE: Print the result character AND 'V' BEFORE the INT 10h mode-set.
; After entering graphics mode, INT 10h/0Eh is unreliable on real hardware.

    ; Zero scratch buffer at 0x20000
    mov  ax, VBE_SCRATCH_SEG
    mov  es, ax
    xor  di, di
    mov  cx, 128
    xor  ax, ax
    rep  stosw

    ; Query mode info
    mov  ax, VBE_SCRATCH_SEG
    mov  es, ax
    xor  di, di
    mov  ax, 0x4F01
    mov  cx, VESA_MODE
    int  0x10

    ; Snapshot fields before segment restore
    mov  si, ax                         ; return code
    mov  bx, word  [es:di + 0]          ; ModeAttributes
    mov  edx, dword [es:di + 40]        ; PhysBasePtr

    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x6000

    mov  dword [0x0600], 0

    cmp  si, 0x004F
    jne  .path_vga

    test bx, 0x0001
    jz   .path_vga

    test bx, 0x0080
    jz   .path_banked

    cmp  edx, 0x00100000
    jb   .path_banked
    cmp  edx, 0xFFFFFFFF
    je   .path_banked

    ; ── LFB path: print status, then set mode ─────────────────────────────────
    mov  dword [0x0600], edx
    screen_char 'L'
    screen_char 'V'             ; <-- printed while still in text mode
    mov  ax, 0x4F02
    mov  bx, 0x4000 | VESA_MODE
    int  0x10                   ; enter graphics mode HERE
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x6000
    cmp  ax, 0x004F
    je   .vesa_done
    mov  dword [0x0600], 0      ; mode-set failed, clear FB
    jmp  .vesa_done

    ; ── Banked path ───────────────────────────────────────────────────────────
.path_banked:
    mov  dword [0x0600], 0
    screen_char 'B'
    screen_char 'V'             ; <-- printed while still in text mode
    mov  ax, 0x4F02
    mov  bx, VESA_MODE
    int  0x10                   ; enter graphics mode HERE
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x6000
    jmp  .vesa_done

    ; ── VGA mode 13h fallback ─────────────────────────────────────────────────
.path_vga:
    mov  dword [0x0600], 0
    screen_char 'G'
    screen_char 'V'             ; <-- printed while still in text mode
    mov  ax, 0x0013
    int  0x10                   ; enter graphics mode HERE
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x6000

.vesa_done:
    ; *** DO NOT call screen_char here or anywhere below ***
    ; We are now in a graphics mode. INT 10h/0Eh is dead on real hardware.

; ── A20 ──────────────────────────────────────────────────────────────────────
    cli

    ; Method 1: BIOS
    mov  ax, 0x2401
    int  0x15
    jnc  .a20_verify

    ; Method 2: KBC
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

; ── A20 verify ───────────────────────────────────────────────────────────────
; Use 0x0000:0x0600 as canary (we own this word – it holds the FB address).
; Alias: 0xFFFF:0x0610 = 0xFFFF0 + 0x0610 = 0x100600.
; With A20 off, 0x100600 wraps to 0x000600 → reads our canary.
; With A20 on,  0x100600 is real extended RAM → different value.
.a20_verify:
    xor  ax, ax
    mov  ds, ax
    mov  es, ax

    mov  eax, dword [ds:0x0600]         ; save current FB address
    push eax                            ; save on stack

    mov  dword [ds:0x0600], 0xCAFEBABE ; write canary
    io_delay
    io_delay

    mov  ax, 0xFFFF
    mov  es, ax
    mov  eax, dword [es:0x0610]         ; read alias

    ; Restore saved FB address
    xor  bx, bx
    mov  ds, bx                         ; DS = 0
    pop  ebx
    mov  dword [ds:0x0600], ebx

    xor  ax, ax
    mov  ds, ax
    mov  es, ax

    cmp  eax, 0xCAFEBABE
    jne  .a20_on                        ; A20 is on

    ; Still off – try port 0x92
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
