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

    _image_end = .; /* end of what the loader reads from disk */

    /* Zeroed data lives at 1 MB (it isn't in the disk image; the loader
     * clears it, A20 is on). The back buffer follows it, sized for the
     * screen mode at run time; the kernel heap starts after that. The
     * space it left below 640 KB is a second heap (lib/alloc.c). */
    .bss 0x100000 (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }
    _backbuf = ALIGN(4096);

    /* Unwind tables / notes: nothing in a freestanding kernel reads them,
     * and below 640 KB every byte counts (.eh_frame alone was ~97 KB). */
    /DISCARD/ : {
        *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note .note.*)
    }

    _heap_start = _backbuf; /* + the back buffer: see kernel.c */
    _low_heap_start = ALIGN(_image_end, 16);

    /*
     * Stack placed just below VGA. Code + data can grow to 0x88000
     * (~480KB from the load address) before hitting it.
     * _kstack_top = 0x98000, leaving 32KB gap before VGA at 0xA0000.
     */
    .kstack 0x88000 (NOLOAD) : {
        _kstack_bottom = .;
        . += 0x10000;       /* 64KB */
        _kstack_top = .;    /* = 0x98000 */
    }

    ASSERT(_kstack_top <= 0xA0000, "Stack overflows into VGA!")
    ASSERT(_image_end <= _kstack_bottom, "Kernel code/data overlaps the stack at 0x88000!")
    ASSERT(_low_heap_start + 0x4000 <= _kstack_bottom, "No room for the low heap")
}
