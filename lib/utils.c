#include "utils.h"
#include "lib/types.h"

void clear_screen_fast(uint8_t color) {
    uint8_t* video_mem = (uint8_t*)0xA0000;
    uint32_t screen_size = 320 * 200;

    for (uint32_t i = 0; i < screen_size; i++) {
        video_mem[i] = color;
    }
}

void delay(int count) {
    volatile int i;
    for (i = 0; i < count; i++) {
        __asm__("nop");
    }
}

unsigned char inb(unsigned short port) {
    unsigned char ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outb(unsigned short port, unsigned char val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
