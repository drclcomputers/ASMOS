ENTRY(_start)
SECTIONS {
    . = 0x10000;

    .text : {
        build/loader.o(.text)
        build/interrupts/interrupt.o(.text)
        *(.text)
    }

    .data   : { *(.data)   }
    .rodata : { *(.rodata) }

    .bss : {
        _bss_start = .;
        *(.bss)
        *(COMMON)
        _bss_end = .;
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
}
