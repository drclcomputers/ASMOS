#include "interrupts/idt.h"
#include "lib/core.h"
#include "lib/memory.h"

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
    if (pit_ticks % 100 == 0)
        pit_seconds++;
}

static void pit_init(void) {
    outb(PIT_CMD, 0x36);
    outb(PIT_CHANNEL0, PIT_DIVISOR & 0xFF);
    outb(PIT_CHANNEL0, (PIT_DIVISOR >> 8) & 0xFF);
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));

    uint32_t spurious_addr = (uint32_t)isr_spurious;
    for (int i = 0; i < IDT_SIZE; i++)
        idt_set_gate(i, spurious_addr);

    idt_set_gate(IRQ0_VECTOR, (uint32_t)isr_timer);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));

    pic_remap();

    outb(PIC1_DATA, 0xFE);
    outb(PIC2_DATA, 0xFF);

    pit_init();

    __asm__ volatile("sti");
}
