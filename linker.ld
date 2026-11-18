ENTRY(_start)
SECTIONS {
    /* The kernel runs at 1 MB. It is the file /SYSTEM/KERNEL.SYS: stage 2
     * (boot/stage2.asm) reads it 64 KB at a time below 1 MB and copies
     * each piece here, then jumps to _start. No size limit but the RAM. */
    . = 0x100000;

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

    /* Zeroed data follows (not in the disk image; the loader clears it),
     * then the kernel heap. */
    .bss ALIGN(4096) (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }
    _heap_start = ALIGN(_bss_end, 4096);

    /* Below 640 KB, where stage 2 read the image: once it is copied, the back
     * buffer (sized for the screen mode at run time) and a second heap
     * (lib/alloc.c) up to the stack. */
    _backbuf = 0x10000;
    _low_heap_start = 0x10000;

    /* Unwind tables / notes: nothing in a freestanding kernel reads them. */
    /DISCARD/ : {
        *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note .note.*)
    }

    /* Stack just below VGA, under the EBDA. */
    .kstack 0x88000 (NOLOAD) : {
        _kstack_bottom = .;
        . += 0x10000;       /* 64KB */
        _kstack_top = .;    /* = 0x98000 */
    }

    ASSERT(_kstack_top <= 0xA0000, "Stack overflows into VGA!")
    /* the biggest back buffer (800x600) between 0x10000 and the stack; in
     * the smaller modes the rest is a second heap (kernel.c) */
    ASSERT(0x10000 + 800 * 600 + 0x1000 <= _kstack_bottom,
           "No room below the stack for the back buffer")
}
