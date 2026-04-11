#include "lib/io.h"

unsigned char inb(unsigned short port) {
    unsigned char ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

unsigned short inw(unsigned short port) {
    unsigned short ret;
    asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outb(unsigned short port, unsigned char val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void outw(unsigned short port, unsigned short val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

void cpu_halt(void) {
    asm volatile ("hlt");
}

void cpu_idle(void) {
    asm volatile ("sti; hlt");
}

void cpu_sleep_ms(uint32_t ms) {
    volatile uint32_t count = ms * 1000;
    while (count--) {
        asm volatile ("nop");
    }
}

void cpu_shutdown(void) {
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}
