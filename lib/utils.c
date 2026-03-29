#include "lib/utils.h"
#include "lib/types.h"

void blit(void) {
    uint32_t* src = (uint32_t*)0x100000;
    uint32_t* dst = (uint32_t*)0xA0000;
    for (int i = 0; i < 320*200/4; i++)
        dst[i] = src[i];
}

void clear_screen(uint8_t color) {
    uint32_t* buf = (uint32_t*)BACKBUF;
    uint32_t val = color | (color << 8) | (color << 16) | (color << 24);
    for (uint32_t i = 0; i < 320 * 200 / 4; i++)
        buf[i] = val;
}

void delay(int count) {
    volatile int i;
    for (i = 0; i < count; i++) {
        __asm__("nop");
    }
}
