ENTRY(_start)
SECTIONS {
    . = 0x10000;

    /* code + constants first: app contexts map them read-only (lib/vmm.c) */
    _ro_start = .;
    .text : {
        build/boot/loader.o(.text)
        build/interrupts/interrupt.o(.text)
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    _ro_end = .;

    .data   : { *(.data .data.*)     }

    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }

    /* Unwind tables / notes: nothing in a freestanding kernel reads them,
     * and below 640 KB every byte counts (.eh_frame alone was ~97 KB). */
    /DISCARD/ : {
        *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note .note.*)
    }

    _heap_start = ALIGN(16);

    /*
     * Stack placed just below VGA. Kernel BSS can now grow to 0x88000
     * (~480KB from load address) before hitting the stack.
     * _kstack_top = 0x98000, leaving 32KB gap before VGA at 0xA0000.
     */
    .kstack 0x88000 (NOLOAD) : {
        _kstack_bottom = .;
        . += 0x10000;       /* 64KB */
        _kstack_top = .;    /* = 0x98000 */
    }

    ASSERT(_kstack_top <= 0xA0000, "Stack overflows into VGA!")
    ASSERT(_bss_end <= _kstack_bottom, "Kernel image/BSS overlaps the stack at 0x88000!")
}
