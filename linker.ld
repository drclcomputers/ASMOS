/* linker.ld */
ENTRY(_start)
SECTIONS {
    . = 0x10000;

    .text : {
        build/loader.o(.text)
        build/interrupts/interrupt.o(.text)
        *(.text)
    }

    .data : {
        *(.data)
    }

    .rodata : {
        *(.rodata)
    }

    .bss : {
        _bss_start = .;
        *(.bss)
        *(COMMON)
        _bss_end = .;
    }

    _heap_start = ALIGN(4);

    /* ── Kernel stack now lives at the top of low memory ── */
    .kstack 0x3F0000 (NOLOAD) : {
        _kstack_bottom = .;
        . += 0x8000;
        _kstack_top = .;
    }

    /* Only a safety check, but we have plenty of room now */
    ASSERT(_bss_end < 0x3F0000, "ERROR: Kernel BSS too large – reduce code")
}
