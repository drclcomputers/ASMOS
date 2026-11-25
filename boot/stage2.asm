; Copyright (c) 2026 @drclcomputers. All rights reserved.
;
; This work is licensed under the terms of the MIT license.
; For a copy, see <https://opensource.org/licenses/MIT>.
;
; ─────────────────────────────────────────────────────────────────────────────
; ASMOS stage 2, at 0x7E00 (the reserved sectors after the VBR): measures the
; RAM, enables A20, finds /SYSTEM/KERNEL.SYS, copies it to 1 MB 64 KB at a
; time in unreal mode, sets the screen mode (byte 4 of this sector), and
; enters 32-bit protected mode. Errors before the mode switch are text.
; ─────────────────────────────────────────────────────────────────────────────
[bits 16]
[org 0x7E00]

%include "boot/bootdefs.inc"

%macro io_delay 0
    call _io_delay_impl
%endmacro

    jmp  short entry
; ── Video mode setting, at a fixed place: "VM" + mode at bytes 2-4 of the
; first stage-2 sector (volume sector 1). Settings > System rewrites the mode
; byte (config/video_cfg.c), so it must stay here. The VBR checks the "VM".
;   0 = 320x200x256 (mode 13h), 1 = 640x400x16 (mode 12h, 400 lines),
;   2 = 640x480x16 (mode 12h), 3 = 800x600x16 (SVGA mode 6Ah / VESA 102h),
;   4 = 320x200x16 (mode 0Dh), 5..7 = 640x400, 640x480, 800x600 in 256
;   colours (VESA 100h, 101h, 103h, or the card's own numbers)
; What the machine can take limits it (step 5).
    db 'V', 'M'
video_mode db 0

entry:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00
    sti
    cld
    mov  si, msg_hello
    call print

; ── 1. RAM (INT 15h, AH=88h): KB above 1 MB, up to 64 MB ───────────────────
    mov  dword [BOOTINFO_RAM], 0
    mov  ah, 0x88
    int  0x15
    jc   .mem_done
    test ax, ax
    jz   .mem_done
    movzx eax, ax
    shl  eax, 10
    add  eax, 0x100000
    mov  [BOOTINFO_RAM], eax
.mem_done:
    xor  ax, ax
    mov  ds, ax
    mov  es, ax

; ── 2. A20 ───────────────────────────────────────────────────────────────────
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
    sti
    cld
    xor  ax, ax
    mov  ds, ax
    mov  es, ax

; ── 3. Find /SYSTEM/KERNEL.SYS ──────────────────────────────────────────────
    call volume_layout
    xor  ax, ax                 ; in the root directory
    mov  si, name_system
    call find_entry
    jc   no_kernel
    test byte [ent_attr], 0x10  ; a folder
    jz   no_kernel
    mov  ax, [ent_cluster]
    mov  si, name_kernel
    call find_entry
    jc   no_kernel
    test byte [ent_attr], 0x10  ; not a folder
    jnz  no_kernel

; ── 4. Read it to 1 MB ──────────────────────────────────────────────────────
    call load_kernel

; for the kernel (bootdefs.inc)
    mov  al, [VBR_DRIVE]
    mov  [BOOTINFO_DRIVE], al
    mov  al, [VBR_USE_LBA]
    mov  [BOOTINFO_LBA], al
    mov  eax, [BPB_HIDDEN]
    mov  [BOOTINFO_PART], eax

; ── 5. Screen mode ───────────────────────────────────────────────────────────
; The mode asked for (video_mode) if the machine allows it, else the nearest
; one it does (smaller first, the same colours first; 320x200 in 256). Allowed:
;   under 3 MB              320x200 only (two 640-wide frames don't fit)
;   a 386                   up to 640x480 (800x600 is too many pixels)
;   a 486 or better, 4 MB   800x600 too, if the video card has the mode
;   256 colours above 320x200: the card's VESA BIOS has the mode, and it can
;   be reached (a linear frame buffer, or a window to write through)
; (3 and 4 MB with some slack: BIOSes keep a little for themselves.)
; The allowed modes go to 0x0603, the rest to 0x0620.. (bootdefs.inc).
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  di, BOOTINFO_VFLAGS        ; 0x0621..0x0631: nothing found yet
    mov  cx, 0x11
    xor  al, al
    cld
    rep  stosb
    mov  byte [BOOTINFO_VMARK], 'V'
    xor  bx, bx                     ; bl = the largest size (0..3)
    pushfd                          ; a 486 can flip AC (EFLAGS bit 18),
    pop  eax                        ; a 386 can't
    mov  ecx, eax
    xor  eax, 0x40000
    push eax
    popfd
    pushfd
    pop  eax
    push ecx
    popfd
    xor  eax, ecx
    test eax, 0x40000
    jz   .cpu_known
    or   byte [BOOTINFO_VFLAGS], VF_486
.cpu_known:
    mov  eax, [BOOTINFO_RAM]
    cmp  eax, 0x2C0000              ; "3 MB"
    jb   .limit_known
    mov  bl, 2
    test byte [BOOTINFO_VFLAGS], VF_486
    jz   .limit_known
    cmp  eax, 0x3C0000              ; "4 MB"
    jb   .limit_known
    mov  bl, 3
.limit_known:
    mov  [v_maxsize], bl
    call vbe_probe
    ; the allowed modes: no bigger than that; 256 colours above 320x200
    ; only with a VESA mode of that size
    xor  dl, dl                     ; dl = the mask
    xor  si, si                     ; si = the mode
.allow:
    mov  al, [mode_size + si]
    cmp  al, [v_maxsize]
    ja   .allow_next
    cmp  byte [mode_256 + si], 0
    je   .allow_yes
    test al, al
    jz   .allow_yes
    movzx ax, al
    bt   word [BOOTINFO_V256], ax
    jnc  .allow_next
.allow_yes:
    bts  dx, si
.allow_next:
    inc  si
    cmp  si, 8
    jb   .allow
    mov  [BOOTINFO_VMAX], dl
    ; the mode: the one asked for, else the nearest allowed (320x200x256
    ; always is, so it ends)
    movzx bx, byte [video_mode]
    cmp  bx, 8
    jb   .asked_ok
    xor  bx, bx
.asked_ok:
    mov  ch, [mode_256 + bx]        ; ch = 1: 256 colours asked for
    mov  cl, [mode_size + bx]       ; cl = the size, then smaller
    mov  dh, cl                     ; dh = the size asked for
.pick:
    test cl, cl                     ; down to 320x200 (not asked for): 256
    jnz  .pick_go                   ; colours first, as it always was
    test dh, dh
    jz   .pick_go
    mov  ch, 1
.pick_go:
    mov  al, ch
    call pick_one
    jnc  .picked
    mov  al, ch
    xor  al, 1
    call pick_one
    jnc  .picked
    dec  cl
    jns  .pick
    xor  al, al
.picked:
    cmp  al, 4
    je   .setup_0d
    ja   .setup_vbe8
.setup_16:                          ; (al = 0..3)
    cmp  al, 3
    je   .setup_800
    cmp  al, 2
    je   .setup_480
    cmp  al, 1
    je   .setup_mode_x
    jmp  .setup_mode_13h

; ── 256 colours above 320x200: the card's VESA mode (5..7 in al) ────────────
.setup_vbe8:
    mov  [v_mode], al
    movzx si, al
    movzx si, byte [mode_size + si]
    shl  si, 1
    mov  cx, [vbe_modes + si]
    call vbe_info
    jc   .vbe8_fail
    mov  [BOOTINFO_VBEMODE], cx
    mov  ax, [VBE_MINFO + 16]       ; bytes a line
    mov  [BOOTINFO_PITCH], ax
    mov  ax, [VBE_MINFO + 4]        ; granularity, KB
    mov  [BOOTINFO_GRAN], ax
    mov  ax, [VBE_MINFO + 6]        ; window size, KB
    mov  [BOOTINFO_WINKB], ax
    call vbe_window                 ; al = the window to write through
    mov  [BOOTINFO_VWIN], al
    mov  [BOOTINFO_WINSEG], dx
    call vbe_lfb
    mov  [BOOTINFO_LFB], eax
    mov  bx, cx
    test eax, eax
    jz   .vbe8_set
    or   bx, 0x4000                 ; the linear frame buffer
.vbe8_set:
    push bx
    mov  ax, 0x4F02
    int  0x10
    pop  bx
    xor  dx, dx
    mov  ds, dx
    mov  es, dx
    cmp  ax, 0x004F
    je   .vbe8_ok
    test bx, 0x4000                 ; the linear one refused: the window
    jz   .vbe8_fail
    mov  cx, [BOOTINFO_VBEMODE]
    call vbe_info
    jc   .vbe8_fail
    call vbe_window
    jc   .vbe8_fail
    mov  dword [BOOTINFO_LFB], 0
    and  bx, 0x3FFF
    jmp  .vbe8_set
.vbe8_fail:                         ; not here after all: 16 colours
    mov  word [BOOTINFO_VBEMODE], 0
    movzx ax, byte [v_mode]
    btr  word [BOOTINFO_VMAX], ax
    sub  al, 4                      ; 5..7 -> 1..3, the same size
    movzx dx, al
    btr  word [BOOTINFO_V256], dx
    jmp  .setup_16
.vbe8_ok:
    movzx si, byte [v_mode]
    movzx si, byte [mode_size + si]
    shl  si, 1
    mov  ax, [size_w + si]
    mov  [BOOTINFO_WIDTH], ax
    mov  ax, [size_h + si]
    mov  [BOOTINFO_HEIGHT], ax
    mov  al, [v_mode]
    mov  [BOOTINFO_VMODE], al
    jmp  .video_setup_done

; ── 320×200×16: the BIOS's mode 0Dh, planar (40 bytes a line per plane) ────
.setup_0d:
    mov  ax, 0x000D
    int  0x10
    mov  cx, 4000                   ; 8 000 bytes
    mov  bx, 320
    mov  si, 200
    mov  dl, 4
    jmp  .planar_clear

; ── 800×600×16: the SVGA BIOS's mode, planar like 640x480 (100 bytes a
; line per plane, 60 000 bytes: inside the 64 KB window, no banks) ─────────
; Mode 6Ah first: the old common number, set through the plain VGA
; registers on every BIOS. VESA's number (102h) only if the card refuses it
; (some BIOSes do 102h their own way: QEMU's leaves the screen black).
.setup_800:
    mov  ax, 0x006A
    int  0x10
    mov  ah, 0x0F                   ; did it take?
    int  0x10
    cmp  al, 0x6A
    je   .ok_800
    mov  ax, 0x4F02                 ; VESA mode 102h
    mov  bx, 0x0102
    int  0x10
    cmp  ax, 0x004F
    je   .ok_800
    xor  ax, ax                     ; not on this card: 640x480
    mov  ds, ax
    or   byte [BOOTINFO_VFLAGS], VF_NO800
    and  byte [BOOTINFO_VMAX], 0xF7
    jmp  .setup_480
.ok_800:
    mov  cx, 30000                  ; 60 000 bytes, all 4 planes at once
    mov  bx, 800
    mov  si, 600
    mov  dl, 3
    jmp  .planar_clear

; ── 640×480×16: the BIOS's mode 12h as it is ─────────────────────────────────
.setup_480:
    mov  ax, 0x0012
    int  0x10
    mov  cx, 19200                  ; 38 400 bytes
    mov  bx, 640
    mov  si, 480
    mov  dl, 2

; cx words to clear, bx x si pixels, dl the mode
.planar_clear:
    push dx
    mov  dx, 0x03C4
    mov  al, 0x02                   ; Map Mask: all 4 planes
    out  dx, al
    inc  dx
    mov  al, 0x0F
    out  dx, al
    pop  dx
    mov  ax, 0xA000
    mov  es, ax
    xor  di, di
    xor  ax, ax
    cld
    rep  stosw
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  [BOOTINFO_VMODE], dl
    mov  [BOOTINFO_WIDTH], bx
    mov  [BOOTINFO_HEIGHT], si
    jmp  .video_setup_done

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

; ── 640x400x16 (planar): BIOS mode 12h sets up the sequencer, GC, AC and
;    palette; then only the vertical timing changes, to 400 lines at 70 Hz
;    (25.175 MHz clock). A plane is 80 bytes a line (CRTC offset 28h words).

.setup_mode_x:
    mov  ax, 0x0012          ; 640x480x16
    int  0x10

    mov  dx, 0x03D4          ; unlock CRTC 0-7 (bit 7 of reg 11h)
    mov  al, 0x11
    out  dx, al
    inc  dx
    in   al, dx
    and  al, 0x7F
    out  dx, al
    dec  dx

    mov  dx, 0x03C2          ; Misc Output: 400-line sync polarity, before
    mov  al, 0xA3            ; the CRTC timing
    out  dx, al

; CRTC: total 448 lines (C0h + overflow 1Fh), display 400 (8Fh), blank from
; 400 (90h), retrace 412-414 (9Ch, 8Eh); horizontal timing as mode 12h.

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

; clear the 4 planes at once (Map Mask 0Fh)
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
    mov  cx, 0x7D00         ; 32000 words
    rep  stosw

; Store mode info
    xor  ax, ax
    mov  ds, ax
    mov  byte [0x0602], 0x01        ; mode = Mode X (EGA planar 16c)
    mov  word [0x0604], 640         ; width
    mov  word [0x0606], 400         ; height

.video_setup_done:
; ── 5b. APM: connect its 32-bit interface, so the kernel can switch the
; machine off (lib/apm.c). Only real mode can connect it; done last, when
; the disk reads are over. ──────────────────────────────────────────────────
    mov  word [BOOTINFO_APM], 0
    pushad                      ; the APM BIOS may change any register
    push ds
    push es
    mov  ax, 0x5300             ; installed?
    xor  bx, bx
    int  0x15
    jc   .apm_end
    cmp  bx, 0x504D             ; "PM"
    jne  .apm_end
    test cl, 0x02               ; has the 32-bit protected-mode interface
    jz   .apm_end
    mov  ax, 0x5304             ; disconnect any interface (may fail)
    xor  bx, bx
    int  0x15
    mov  ax, 0x5303             ; connect 32-bit protected mode
    xor  bx, bx
    xor  esi, esi
    xor  edi, edi
    int  0x15
    jc   .apm_end
    mov  [BOOTINFO_APMCS], ax
    mov  [BOOTINFO_APMENT], ebx
    mov  [BOOTINFO_APMC16], cx
    mov  [BOOTINFO_APMDS], dx
    mov  ax, 0x5300             ; again: version and flags, now connected
    xor  bx, bx
    int  0x15
    jc   .apm_end
    mov  [BOOTINFO_APMFL], cx
    mov  [BOOTINFO_APM], ax     ; last: the rest is valid
.apm_end:
    pop  es
    pop  ds
    popad
    cld                         ; (and the direction flag)
    xor  ax, ax
    mov  ds, ax
    mov  es, ax

; ── 6. Protected mode, then the kernel ──────────────────────────────────────
    cli
    lgdt [gdt_descriptor]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  dword 0x08:pm_entry    ; 32-bit far jump: flushes the prefetch

[bits 32]
pm_entry:
    mov  eax, 0x10
    mov  ds, eax
    mov  es, eax
    mov  ss, eax
    mov  esp, 0x7C00
    jmp  0x08:KERNEL_ENTRY
[bits 16]

; ═════════════════════════════════════════════════════════════════════════════
; The FAT volume
; ═════════════════════════════════════════════════════════════════════════════

; volume_layout: where the FATs, root and data start on the disk; FAT12/16
volume_layout:
    movzx eax, word [BPB_RESERVED]
    add  eax, [BPB_HIDDEN]
    mov  [fat_lba], eax
    movzx ecx, byte [BPB_FATS]
    movzx edx, word [BPB_FAT_SECS]
    imul ecx, edx
    add  eax, ecx
    mov  [root_lba], eax
    movzx ecx, word [BPB_ROOT_ENTS]
    shl  ecx, 5
    add  ecx, 511
    shr  ecx, 9
    mov  [root_secs], cx
    add  eax, ecx
    mov  [data_lba], eax
    ; clusters = (total - (data - hidden)) / per cluster; FAT12 if < 4085
    movzx eax, word [BPB_TOTAL16]
    test eax, eax
    jnz  .t
    mov  eax, [BPB_TOTAL32]
.t:
    mov  ecx, [data_lba]
    sub  ecx, [BPB_HIDDEN]
    sub  eax, ecx
    xor  edx, edx
    movzx ecx, byte [BPB_SEC_CLUS]
    div  ecx
    cmp  eax, 4085
    setb byte [fat12]
    ret

; cluster_lba: cluster AX -> EAX, its first sector on the disk
cluster_lba:
    push ecx
    movzx eax, ax
    sub  eax, 2
    movzx ecx, byte [BPB_SEC_CLUS]
    imul eax, ecx
    add  eax, [data_lba]
    pop  ecx
    ret

; next_cluster: cluster AX -> AX, the next one in its chain; CF = no more
next_cluster:
    push ebx
    push ecx
    push esi
    movzx ebx, ax
    mov  esi, ebx
    cmp  byte [fat12], 0
    jne  .f12
    shl  esi, 1                 ; FAT16: 2 bytes an entry
    jmp  .off
.f12:
    shr  esi, 1                 ; FAT12: 1.5
    add  esi, ebx
.off:
    mov  eax, esi
    shr  eax, 9
    add  eax, [fat_lba]
    cmp  eax, [fat_cached]
    je   .have
    mov  [fat_cached], eax
    push bx
    mov  bx, FAT_BUF            ; two sectors: an entry can straddle them
    mov  cx, 2
    call read_disk
    pop  bx
.have:
    and  si, 511
    mov  ax, [FAT_BUF + si]
    cmp  byte [fat12], 0
    jne  .v12
    cmp  ax, 0xFFF8
    jae  .end
    jmp  .check
.v12:
    test bl, 1
    jz   .even
    shr  ax, 4
    jmp  .m12
.even:
    and  ax, 0x0FFF
.m12:
    cmp  ax, 0x0FF8
    jae  .end
.check:
    cmp  ax, 2                  ; free / reserved: a broken chain
    jb   .end
    clc
    jmp  .out
.end:
    stc
.out:
    pop  esi
    pop  ecx
    pop  ebx
    ret

; scan_sector: read directory sector EAX, look for the name at [fe_name]
;   [fe_result] = 0 not here, 1 found (ent_*), 2 the directory ends here
scan_sector:
    pushad
    mov  [diag_lba], eax
    mov  bx, DIR_BUF
    mov  cx, 1
    call read_disk
    xor  ax, ax                 ; cmpsb: DS:SI against ES:DI
    mov  ds, ax
    mov  es, ax
    cld
    mov  di, DIR_BUF
    mov  dx, 16
.e:
    mov  al, [di]
    test al, al
    jz   .end
    cmp  al, 0xE5               ; deleted
    je   .skip
    test byte [di + 11], 0x08   ; a volume label, or a long-name part
    jnz  .skip
    push di
    mov  si, [fe_name]
    mov  cx, 11
    repe cmpsb
    pop  di
    je   .found
.skip:
    add  di, 32
    dec  dx
    jnz  .e
    mov  byte [fe_result], 0
    popad
    ret
.end:
    mov  byte [fe_result], 2
    popad
    ret
.found:
    mov  al, [di + 11]
    mov  [ent_attr], al
    mov  ax, [di + 26]
    mov  [ent_cluster], ax
    mov  eax, [di + 28]
    mov  [ent_size], eax
    mov  byte [fe_result], 1
    popad
    ret

; find_entry: SI = an 8.3 name ("KERNEL  SYS"), AX = the directory's first
; cluster (0 = the root). CF = not found; else ent_attr/cluster/size.
find_entry:
    mov  [fe_name], si
    test ax, ax
    jnz  .chain
    mov  eax, [root_lba]
    mov  cx, [root_secs]
.r:
    call scan_sector
    cmp  byte [fe_result], 0
    jne  .done
    inc  eax
    loop .r
    stc
    ret
.chain:
    mov  [fe_clus], ax
.c:
    call cluster_lba
    movzx cx, byte [BPB_SEC_CLUS]
.cs:
    call scan_sector
    cmp  byte [fe_result], 0
    jne  .done
    inc  eax
    loop .cs
    mov  ax, [fe_clus]
    call next_cluster
    jc   .no
    mov  [fe_clus], ax
    jmp  .c
.no:
    stc
    ret
.done:
    cmp  byte [fe_result], 1
    je   .yes
    stc
    ret
.yes:
    clc
    ret

; load_kernel: the file found (ent_cluster, ent_size) to KERNEL_ENTRY.
; Clusters next to each other go in one read, up to 64 KB.
load_kernel:
    mov  ax, [ent_cluster]
    cmp  ax, 2
    jb   damaged
    mov  [ld_cur], ax
    mov  dword [ld_dest], KERNEL_ENTRY
.run:
    mov  ax, [ld_cur]
    mov  [run_start], ax
    mov  word [run_secs], 0
    movzx bx, byte [BPB_SEC_CLUS]
.grow:
    add  [run_secs], bx
    mov  ax, [ld_cur]
    call next_cluster
    jnc  .more
    xor  ax, ax                 ; the last one
.more:
    mov  [ld_next], ax
    test ax, ax
    jz   .read
    mov  cx, [ld_cur]
    inc  cx
    cmp  ax, cx                 ; right after this one?
    jne  .read
    mov  cx, [run_secs]
    add  cx, bx
    cmp  cx, 128                ; still within 64 KB?
    ja   .read
    mov  [ld_cur], ax
    jmp  .grow
.read:
    mov  ax, [run_start]
    call cluster_lba
    mov  cx, [run_secs]
    push es
    mov  bx, LOAD_SEG
    mov  es, bx
    xor  bx, bx
    call read_disk
    pop  es
    ; up to 1 MB+ (unreal mode: set again each time, a BIOS may undo it)
    call go_unreal
    movzx ecx, word [run_secs]
    shl  ecx, 7                 ; dwords: 512 / 4 per sector
    mov  esi, LOAD_SEG << 4
    mov  edi, [ld_dest]
    a32 rep movsd
    mov  [ld_dest], edi
    mov  al, '.'
    call putc
    mov  ax, [ld_next]
    test ax, ax
    jz   .done
    mov  [ld_cur], ax
    jmp  .run
.done:
    mov  eax, [ld_dest]
    sub  eax, KERNEL_ENTRY
    cmp  eax, [ent_size]        ; the chain was as long as the file?
    jb   damaged
    cmp  dword [ent_size], 0
    je   damaged
    ret

; go_unreal: DS and ES get 4 GB limits (base 0) and stay in real mode
go_unreal:
    push eax
    push bx
    cli
    lgdt [gdt_descriptor]
    mov  eax, cr0
    or   al, 1
    mov  cr0, eax
    jmp  short .pm
.pm:
    mov  bx, 0x10
    mov  ds, bx
    mov  es, bx
    and  al, 0xFE
    mov  cr0, eax
    jmp  short .rm
.rm:
    xor  bx, bx
    mov  ds, bx
    mov  es, bx
    sti
    pop  bx
    pop  eax
    ret

; ═════════════════════════════════════════════════════════════════════════════
; The disk
; ═════════════════════════════════════════════════════════════════════════════

; read_disk: CX sectors from disk sector EAX to ES:BX. The caller keeps the
; whole read inside one 64 KB block (ISA DMA). LBA if the BIOS can, else CHS
; a track at a time. 3 tries a read, then "Disk error".
read_disk:
    pushad
    mov  [rd_lba], eax
    mov  [rd_left], cx
    mov  [rd_off], bx
    mov  [rd_seg], es
.loop:
    cmp  word [rd_left], 0
    je   .done
    mov  di, 3
.try:
    mov  eax, [rd_lba]
    cmp  byte [VBR_USE_LBA], 0
    je   .chs
    mov  cx, [rd_left]
    cmp  cx, 64
    jbe  .l0
    mov  cx, 64
.l0:
    cmp  byte [rd_single], 0
    je   .l1
    mov  cx, 1
.l1:
    mov  [rd_n], cx
    mov  [dap_count], cx
    mov  [dap_lba], eax
    mov  ax, [rd_off]
    mov  [dap_off], ax
    mov  ax, [rd_seg]
    mov  [dap_seg], ax
    mov  si, dap
    mov  ah, 0x42
    mov  dl, [VBR_DRIVE]
    int  0x13
    jmp  .result
.chs:
    xor  edx, edx
    movzx ecx, word [VBR_SPT]
    div  ecx                    ; eax = track, edx = sector - 1
    mov  bx, [VBR_SPT]          ; what is left of this track
    sub  bx, dx
    cmp  bx, [rd_left]
    jbe  .c1
    mov  bx, [rd_left]
.c1:
    cmp  byte [rd_single], 0
    je   .c2
    mov  bx, 1
.c2:
    mov  [rd_n], bx
    mov  cl, dl
    inc  cl
    xor  edx, edx
    movzx esi, word [VBR_HEADS]
    div  esi                    ; eax = cylinder, edx = head
    cmp  eax, 1023
    ja   disk_error             ; CHS can't reach it
    mov  dh, dl
    mov  ch, al
    shl  ah, 6
    or   cl, ah
    mov  es, [rd_seg]
    mov  bx, [rd_off]
    mov  al, [rd_n]
    mov  ah, 0x02
    mov  dl, [VBR_DRIVE]
    int  0x13
.result:
    jnc  .ok
    xor  ax, ax                 ; reset, try again
    mov  dl, [VBR_DRIVE]
    int  0x13
    dec  di
    jnz  .try
    cmp  byte [rd_single], 0    ; one sector at a time, then: some BIOSes
    jne  disk_error             ; take no more
    mov  byte [rd_single], 1
    mov  di, 3
    jmp  .try
.ok:
    xor  ax, ax                 ; some BIOSes leave DS changed, and ES
    mov  ds, ax                 ; (after AH=42h, which doesn't use it):
    mov  es, [rd_seg]           ; the callers still need it
    cld
    movzx eax, word [rd_n]
    add  [rd_lba], eax
    sub  [rd_left], ax
    shl  ax, 9
    add  [rd_off], ax
    jmp  .loop
.done:
    popad
    ret

; ═════════════════════════════════════════════════════════════════════════════
; Messages
; ═════════════════════════════════════════════════════════════════════════════

disk_error:
    mov  si, msg_disk
    jmp  fatal
no_kernel:
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    cld
    mov  si, msg_nokernel
    call print
    ; what was read, so a wrong read can be told from a missing file:
    ; "(SYSTEM     ; sector 0000007F LBA 1; first entry 'ASMOS      ')"
    mov  si, msg_d1
    call print
    mov  si, [fe_name]
    mov  cx, 11
    call print_n
    mov  si, msg_d2
    call print
    mov  eax, [diag_lba]
    call print_hex
    mov  si, msg_d3
    call print
    mov  al, [VBR_USE_LBA]
    add  al, '0'
    call putc
    mov  si, msg_d4
    call print
    mov  si, DIR_BUF
    mov  cx, 11
    call print_n
    mov  si, msg_d5
    jmp  fatal
damaged:
    mov  si, msg_damaged
fatal:
    xor  ax, ax
    mov  ds, ax
    cld
    call print
    mov  si, msg_key
    call print
    xor  ax, ax
    int  0x16
    int  0x19                   ; the BIOS boots again / the next device

print:
    lodsb
    test al, al
    jz   .d
    call putc
    jmp  print
.d:
    ret

; print_n: CX characters at DS:SI, unprintable ones as '.'
print_n:
    lodsb
    cmp  al, ' '
    jb   .dot
    cmp  al, '~'
    jbe  .put
.dot:
    mov  al, '.'
.put:
    call putc
    loop print_n
    ret

; print_hex: EAX in 8 hex digits
print_hex:
    mov  cx, 8
.h:
    rol  eax, 4
    push eax
    and  al, 0x0F
    add  al, '0'
    cmp  al, '9'
    jbe  .p
    add  al, 7
.p:
    call putc
    pop  eax
    loop .h
    ret

putc:
    pusha
    mov  ah, 0x0E
    mov  bx, 7
    int  0x10
    popa
    ret

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

; ── Screen-mode helpers (step 5) ─────────────────────────────────────────────
VBE_CINFO equ 0x9000                ; the card's info (512 bytes): FAT_BUF,
VBE_MINFO equ 0x9200                ; free now; a mode's (256 bytes)

; al = colours (0 = 16, 1 = 256), cl = size -> al = that mode; CF = not
; allowed
pick_one:
    shl  al, 2
    add  al, cl
    movzx si, al
    mov  al, [mode_by + si]
    movzx bx, al
    bt   word [BOOTINFO_VMAX], bx
    cmc
    ret

; cx = a VESA mode -> its info at VBE_MINFO; CF = none
vbe_info:
    pushad
    mov  ax, 0x4F01
    mov  di, VBE_MINFO
    int  0x10
    xor  dx, dx
    mov  ds, dx
    mov  es, dx
    cmp  ax, 0x004F
    popad
    je   .ok
    stc
    ret
.ok:
    clc
    ret

; the mode at VBE_MINFO: eax = its linear frame buffer (VBE 2.0), or 0
vbe_lfb:
    xor  eax, eax
    test byte [BOOTINFO_VFLAGS], VF_VBE2
    jz   .done
    test byte [VBE_MINFO], 0x80
    jz   .done
    mov  eax, [VBE_MINFO + 40]
.done:
    ret

; the mode at VBE_MINFO: a window to write through? al = 0 (A) / 1 (B),
; dx = its segment; CF = none
vbe_window:
    test byte [VBE_MINFO], 0x40     ; VBE 2.0: no windows at all
    jnz  .no
    cmp  word [VBE_MINFO + 4], 0    ; (a granularity is needed)
    je   .no
    xor  al, al
    mov  dx, [VBE_MINFO + 8]
    mov  ah, [VBE_MINFO + 2]        ; A: there and writable?
    and  ah, 5
    cmp  ah, 5
    je   .seg
    inc  al
    mov  dx, [VBE_MINFO + 10]
    mov  ah, [VBE_MINFO + 3]
    and  ah, 5
    cmp  ah, 5
    jne  .no
.seg:
    cmp  dx, 0xA000
    jne  .no
    clc
    ret
.no:
    stc
    ret

; cx = a VESA mode: 256 colours, [v_size]'s size, and reachable? CF = no
vbe_check:
    call vbe_info
    jc   .bad
    mov  ax, [VBE_MINFO]            ; supported, colour, graphics
    and  ax, 0x19
    cmp  ax, 0x19
    jne  .bad
    movzx si, byte [v_size]
    shl  si, 1
    test byte [VBE_MINFO], 0x02     ; the details there? (VBE 1.0 may not)
    jz   .std_only
    cmp  byte [VBE_MINFO + 25], 8   ; 8 bits a pixel, packed
    jne  .bad
    cmp  byte [VBE_MINFO + 27], 4
    jne  .bad
    mov  ax, [size_w + si]
    cmp  [VBE_MINFO + 18], ax
    jne  .bad
    mov  ax, [size_h + si]
    cmp  [VBE_MINFO + 20], ax
    jne  .bad
    jmp  .reach
.std_only:                          ; then only the standard number counts
    cmp  cx, [std_modes + si]
    jne  .bad
.reach:
    call vbe_lfb
    test eax, eax
    jnz  .good
    call vbe_window
    jc   .bad
.good:
    clc
    ret
.bad:
    stc
    ret

; the card's VESA BIOS: a 256-colour mode of each size above 320x200?
; -> BOOTINFO_V256, vbe_modes, the VBE flags
vbe_probe:
    mov  di, VBE_CINFO
    mov  dword [di], 'VBE2'         ; (asks for the 2.0 details)
    mov  ax, 0x4F00
    int  0x10
    xor  dx, dx
    mov  ds, dx
    mov  es, dx
    cmp  ax, 0x004F
    jne  .done
    cmp  dword [VBE_CINFO], 'VESA'
    jne  .done
    or   byte [BOOTINFO_VFLAGS], VF_VBE
    cmp  word [VBE_CINFO + 4], 0x0200
    jb   .v1
    or   byte [BOOTINFO_VFLAGS], VF_VBE2
.v1:
    mov  byte [v_size], 1
.size:
    movzx si, byte [v_size]
    shl  si, 1
    mov  cx, [std_modes + si]       ; the standard number first,
    call vbe_check
    jnc  .found
    lfs  si, [VBE_CINFO + 14]       ; then the card's list
    mov  bp, 256                    ; (no more than that)
.list:
    mov  cx, [fs:si]
    cmp  cx, 0xFFFF
    je   .next
    add  si, 2
    push fs
    push si
    call vbe_check
    pop  si
    pop  fs
    jnc  .found
    dec  bp
    jnz  .list
    jmp  .next
.found:
    movzx si, byte [v_size]
    shl  si, 1
    mov  [vbe_modes + si], cx
    movzx ax, byte [v_size]
    bts  word [BOOTINFO_V256], ax
.next:
    inc  byte [v_size]
    cmp  byte [v_size], 3
    jbe  .size
.done:
    ret

;              320x200  640x400  640x480  800x600
size_w    dw   320,     640,     640,     800
size_h    dw   200,     400,     480,     600
std_modes dw   0x0013,  0x0100,  0x0101,  0x0103  ; VESA's 256-colour numbers
vbe_modes dw   0,       0,       0,       0       ; what the card has
mode_size db   0, 1, 2, 3, 0, 1, 2, 3            ; mode -> size
mode_256  db   1, 0, 0, 0, 0, 1, 1, 1            ; mode -> 256 colours?
mode_by   db   4, 1, 2, 3,  0, 5, 6, 7           ; [colours * 4 + size]
v_maxsize db   0
v_size    db   0
v_mode    db   0

; ── GDT: flat 4 GB code and data ────────────────────────────────────────────
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

msg_hello    db "Starting ASMOS", 0
msg_disk     db 13, 10, "Disk error.", 0
msg_nokernel db 13, 10, "SYSTEM\KERNEL.SYS is missing.", 0
msg_damaged  db 13, 10, "SYSTEM\KERNEL.SYS is damaged.", 0
msg_key      db 13, 10, "Press a key to restart.", 0
msg_d1       db 13, 10, "(", 0
msg_d2       db ": last sector ", 0
msg_d3       db ", LBA ", 0
msg_d4       db ", its first entry '", 0
msg_d5       db "')", 0
name_system  db "SYSTEM     "
name_kernel  db "KERNEL  SYS"

align 4
fat_lba     dd 0
root_lba    dd 0
data_lba    dd 0
fat_cached  dd 0xFFFFFFFF
ent_size    dd 0
diag_lba    dd 0
ld_dest     dd 0
rd_lba      dd 0
root_secs   dw 0
ent_cluster dw 0
fe_name     dw 0
fe_clus     dw 0
ld_cur      dw 0
ld_next     dw 0
run_start   dw 0
run_secs    dw 0
rd_left     dw 0
rd_off      dw 0
rd_seg      dw 0
rd_n        dw 0
fat12       db 0
ent_attr    db 0
fe_result   db 0
rd_single   db 0
align 4
dap:        db 0x10, 0
dap_count   dw 0
dap_off     dw 0
dap_seg     dw 0
dap_lba     dd 0, 0

%if ($-$$) > FAT_BUF - STAGE2_ADDR
  %error "stage 2 would run into its FAT buffer (FAT_BUF)"
%endif
%if ($-$$) > STAGE2_MAX * 512
  %error "stage 2 is over STAGE2_MAX sectors"
%endif
times (($-$$) + 511) / 512 * 512 - ($-$$) db 0
