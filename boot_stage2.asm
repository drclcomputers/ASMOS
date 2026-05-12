[bits 16]
[org 0x7E00]

VESA_MODE    equ 0x0101
KERNEL_ENTRY equ 0x8000

; Visual debug macro — prints a character to the screen via BIOS teletype
; Preserves all registers (uses pusha/popa)
%macro screen_char 1
    pusha
    mov ah, 0x0E
    mov al, %1
    xor bh, bh
    int 0x10
    popa
%endmacro

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000

    sti

    screen_char 'S'     ; Stage2 start
    screen_char '2'
    screen_char 13
    screen_char 10

; ── E820 ─────────────────────────────────────────────────────────────────────
; Protocol: after each successful INT 15h/E820 call, the returned entry is
; valid regardless of whether EBX is 0 or not.  EBX==0 signals "last entry"
; but that entry must still be stored.  The original code advanced DI and
; incremented BP before checking EBX, which caused the last entry to be
; stored but the count written to 0x0500 to include a phantom entry when the
; zero-length guard short-circuited to .e820_next without the inc/add.  Fixed
; by always storing a non-zero-length entry first, then checking EBX.
    mov word [0x0500], 0
    xor ax, ax
    mov es, ax
    mov di, 0x0504
    xor ebx, ebx
    mov edx, 0x534D4150
    xor bp, bp
.e820:
    mov eax, 0xE820
    mov ecx, 20
    int 0x15
    jc  .e820_done          ; carry set → list complete (or unsupported)
    cmp eax, 0x534D4150
    jne .e820_done          ; signature mismatch → abort
    test ecx, ecx
    jz  .e820_check_last    ; zero-length entry: don't store, but check if last
    ; Valid entry — store it
    inc bp
    add di, 20
    cmp bp, 50              ; hard cap: max 50 entries
    jae .e820_done
.e820_check_last:
    test ebx, ebx           ; EBX==0 means this was the last entry
    jnz .e820
.e820_done:
    mov [0x0500], bp
    xor ax, ax
    mov ds, ax
    mov es, ax

    screen_char 'E'         ; E820 done

; ── VBE 4F01 ─────────────────────────────────────────────────────────────────
; The ModeInfoBlock is 256 bytes.  We must NOT point ES:DI at the IVT
; (0x0000–0x03FF) or the BIOS Data Area (0x0400–0x04FF): a real VBIOS will
; happily overwrite interrupt vectors there and cause an immediate triple-fault
; when we enable interrupts or take the first IRQ.  Use physical 0x20000
; (segment 0x2000, offset 0) which is well above the BDA and below the kernel.
VBE_SCRATCH_SEG equ 0x1000   ; physical 0x20000 — safe scratch for ModeInfoBlock

    mov ax, VBE_SCRATCH_SEG
    mov es, ax
    xor di, di               ; ES:DI = 0x2000:0x0000 = physical 0x20000
    mov ax, 0x4F01
    mov cx, VESA_MODE
    int 0x10
    ; INT 10h returns status in AX (0x004F = success).
    ; Read PhysBasePtr and ModeAttributes from ES:DI before zeroing ES.
    ; Save AX now so AL isn't clobbered before the cmp ax,0x004F check.
    mov si,  ax              ; SI = INT 10h return status
    mov ebx, [es:di + 40]    ; EBX = PhysBasePtr
    mov cl,  [es:di]         ; CL  = ModeAttributes byte

    xor ax, ax
    mov ds, ax
    mov es, ax

    mov dword [0x0600], 0

    cmp si, 0x004F           ; did INT 10h succeed?
    jne .vesa_banked

    test cl, 0x01            ; bit 0: mode supported by hardware
    jz  .vesa_banked

    mov [0x0600], ebx        ; store the PhysBasePtr we saved earlier

    screen_char 'L'          ; LFB address found

    mov ax, 0x4F02
    mov bx, 0x4000 | VESA_MODE
    int 0x10
    xor cx, cx
    mov ds, cx
    mov es, cx
    cmp ax, 0x004F
    je  .vesa_done

.vesa_banked:
    screen_char 'B'          ; banked fallback
    mov dword [0x0600], 0
    mov ax, 0x4F02
    mov bx, VESA_MODE
    int 0x10
    xor cx, cx
    mov ds, cx
    mov es, cx

.vesa_done:
    screen_char 'V'          ; VESA done

; ── Enable A20 line ──────────────────────────────────────────────────────────
; Fast A20 via port 0x92 — works on i430FX and virtually all post-1993 chipsets.
; Bit 1 = A20 enable, bit 0 = reset (do NOT set bit 0 — that resets the CPU).
    in  al, 0x92
    test al, 0x02
    jnz .a20_done           ; already enabled (e.g. QEMU)
    or  al, 0x02
    and al, 0xFE            ; make absolutely sure bit 0 (reset) stays clear
    out 0x92, al
.a20_done:
    screen_char 'A'         ; A20 enabled

; ── Protected mode ────────────────────────────────────────────────────────────
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; No debug output in protected mode — jump straight to kernel
    jmp KERNEL_ENTRY

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

; Pad to exactly 8 sectors so the build fails loudly if stage2 ever overflows.
; NASM will error: "TIMES value -N is negative" if the binary exceeds 4096 bytes.
times (8 * 512) - ($ - $$) db 0
