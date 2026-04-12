/* linker.ld */
ENTRY(_start)
SECTIONS {
    . = 0x8000;

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
        *(.bss)
        *(COMMON)
    }
}
