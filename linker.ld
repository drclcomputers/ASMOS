ENTRY(_start)
SECTIONS {
    /* The kernel runs at 1 MB. Stage 1 reads the image (real mode: below
     * 640 KB only) to 0x10000; stage 2, in protected mode, copies it here
     * and jumps to _start. Its size is then limited only by what the boot
     * sectors hold (1000 sectors: see the ASSERT), not by 640 KB. */
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

    /* Below 640 KB, where the image was read: once it is copied, the back
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
    ASSERT(_image_end - 0x100000 <= 1000 * 512,
           "Kernel image over 1000 sectors: the boot sectors hold no more")
    ASSERT(0x10000 + 640 * 400 + 0x10000 <= _kstack_bottom,
           "No room below the stack for the back buffer and the low heap")
}
