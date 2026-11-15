#include "interrupts/idt.h"
#include "lib/core.h"
#include "lib/cpu.h"
#include "lib/fpu_emu.h"
#include "lib/memory.h"
#include "os/appguard.h"
#include "os/watchdog.h"

typedef struct __attribute__((packed)) {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_hi;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;

#define IDT_SIZE 256

static idt_entry_t idt[IDT_SIZE];
static idt_ptr_t idt_ptr;

extern void isr_timer(void);
extern void isr_spurious(void);

static void idt_set_gate(uint8_t n, uint32_t handler) {
    idt[n].offset_lo = (uint16_t)(handler & 0xFFFF);
    idt[n].offset_hi = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].selector = 0x08;
    idt[n].zero = 0;
    idt[n].type_attr = 0x8E;
}

static void pic_remap(void) {
    uint8_t m1 = inb(PIC1_DATA);
    uint8_t m2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);

    outb(PIC1_DATA, IRQ0_VECTOR);
    outb(PIC2_DATA, IRQ0_VECTOR + 8);

    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);

    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    outb(PIC1_DATA, m1);
    outb(PIC2_DATA, m2);
}

volatile uint32_t pit_ticks = 0;
volatile uint32_t pit_seconds = 0;

void pit_tick_handler(void) {
    pit_ticks++;
    if (pit_ticks % 1000 == 0) {
        pit_seconds++;
    }
    wd_tick(); /* last: it may abort a stuck app and not return */
}

static void pit_init(void) {
    outb(PIT_CMD, 0x36);                    // Counter 0, LSB/MSB, Mode 3
    outb(PIT_CHANNEL0, PIT_DIVISOR & 0xFF); // LSB
    outb(PIT_CHANNEL0, PIT_DIVISOR >> 8);
}

static uint16_t s_irq_owned; /* lines handed out by idt_set_irq */

bool idt_irq_in_use(int irq) {
    return irq >= 0 && irq < 16 && (s_irq_owned & (1u << irq));
}

void idt_set_irq(int irq, void (*stub)(void)) {
    if (irq < 0 || irq > 15 || irq == 2)
        return;
    s_irq_owned |= (uint16_t)(1u << irq);
    idt_set_gate(IRQ0_VECTOR + irq, (uint32_t)stub); /* slave is remapped
                                                        right after master */
    if (irq < 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << irq));
    } else {
        outb(PIC2_DATA, inb(PIC2_DATA) & ~(1 << (irq - 8)));
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 2)); /* cascade */
    }
}

void idt_mask_irq(int irq) {
    if (irq >= 0 && irq < 8)
        outb(PIC1_DATA, inb(PIC1_DATA) | (1 << irq));
    else if (irq >= 8 && irq < 16)
        outb(PIC2_DATA, inb(PIC2_DATA) | (1 << (irq - 8)));
}

void idt_emulate_fpu(void) { fpu_emu_install(idt_set_gate); }

void idt_init(void) {
    memset(idt, 0, sizeof(idt));

    uint32_t spurious_addr = (uint32_t)isr_spurious;
    for (int i = 0; i < IDT_SIZE; i++)
        idt_set_gate(i, spurious_addr);

    appguard_install_gates(idt_set_gate);
    if (!cpu_has_fpu()) /* 386/486SX: x87 code runs on the emulator */
        fpu_emu_install(idt_set_gate);
    idt_set_gate(IRQ0_VECTOR, (uint32_t)isr_timer);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));

    pic_remap();

    /* IRQ 0 (timer), 1, 2 (cascade) open; drivers open their own line
     * with idt_set_irq() once they know it. */
    outb(PIC1_DATA, 0xF8);
    outb(PIC2_DATA, 0xFF);

    pit_init();

    __asm__ volatile("sti");
}
