; ─────────────────────────────────────────────────────────────────────────────
; ASMOS stage 2: the reserved sectors after the VBR, run at 0x7E00.
;
; The VBR (vbr.asm) read it and left at 0x7C00 the volume's BPB and how to
; read the drive (bootdefs.inc). Stage 2:
;   1. measures the RAM (INT 15h AH=88h)                          -> 0x0500
;   2. turns on A20, so memory past 1 MB is really there
;   3. finds /SYSTEM/KERNEL.SYS on the FAT12 / FAT16 volume
;   4. reads it 64 KB at a time below 1 MB and copies each piece up to 1 MB
;      in "unreal mode" (real mode with 4 GB segment limits), so the kernel
;      may be any size the RAM allows; its clusters may be anywhere
;   5. sets the screen mode kept in byte 4 of this sector         -> 0x0602
;   6. switches to 32-bit protected mode and jumps to the kernel at 1 MB
; Until 5 the screen is in text mode: a problem is shown as a message.
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
;   2 = 640x480x16 (mode 12h), 3 = 800x600x16 (SVGA mode 6Ah / VESA 102h)
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
; The mode asked for (video_mode), but no more than the machine can take:
;   under 3 MB              320x200 only (two 640-wide frames don't fit)
;   a 386                   up to 640x480 (800x600 is too many pixels)
;   a 486 or better, 4 MB   800x600 too, if the video card has the mode
; (3 and 4 MB with some slack: BIOSes keep a little for themselves.)
; What is allowed goes to 0x0603 for Setup and Settings (bootdefs.inc).
    xor  ax, ax
    mov  ds, ax
    xor  bx, bx                     ; bl = highest mode, bh = flags
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
    or   bh, VMAX_486
.cpu_known:
    mov  eax, [BOOTINFO_RAM]
    cmp  eax, 0x2C0000              ; "3 MB"
    jb   .limit_known
    mov  bl, 2
    test bh, VMAX_486
    jz   .limit_known
    cmp  eax, 0x3C0000              ; "4 MB"
    jb   .limit_known
    mov  bl, 3
.limit_known:
    or   bl, bh
    mov  [BOOTINFO_VMAX], bl
    and  bl, 3
    mov  al, [video_mode]
    cmp  al, bl
    jbe  .mode_known
    mov  al, bl
.mode_known:
    cmp  al, 3
    je   .setup_800
    cmp  al, 2
    je   .setup_480
    cmp  al, 1
    je   .setup_mode_x
    jmp  .setup_mode_13h

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
    or   byte [BOOTINFO_VMAX], VMAX_NO800
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

%if ($-$$) > STAGE2_MAX * 512
  %error "stage 2 is over STAGE2_MAX sectors"
%endif
times (($-$$) + 511) / 512 * 512 - ($-$$) db 0
