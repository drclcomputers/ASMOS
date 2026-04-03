[bits 32]

extern pit_tick_handler

global isr_timer
isr_timer:
    pusha                       ; save registers

    call pit_tick_handler

    mov al, 0x20
    out 0x20, al

    popa                        ; restore registers
    iret

global isr_spurious
isr_spurious:
    pusha
    mov al, 0x20
    out 0x20, al
    popa
    iret
