[bits 32]

extern pit_tick_handler
extern sb16_irq_handler
extern ne2000_irq_handler

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

global isr_sb16
isr_sb16:
    pusha
    call sb16_irq_handler
    popa
    iret

global isr_ne2000
isr_ne2000:
    pusha
    call ne2000_irq_handler
    popa
    iret
