#include "drivers/gpu.h"
#include "config/config.h"
#include "lib/core.h"
#include "lib/memory.h"

extern int g_video_mode;

#define MODEX_FB 0x000A0000U
#define VGA_SEQ_ADDR 0x03C4
#define VGA_SEQ_DATA 0x03C5
#define SEQ_MAP_MASK 0x02

uint32_t g_vesa_fb = MODEX_FB;

void gpu_init(void) {
    g_vesa_fb = MODEX_FB;
}

static void gpu_blit_mode_13h(void) {
    const uint8_t *src = (const uint8_t *)BACKBUF;
    uint8_t *fb = (uint8_t *)MODEX_FB;
    int size = SCREEN_WIDTH * SCREEN_HEIGHT;
    for (int i = 0; i < size; i++)
        fb[i] = src[i];
}

static void gpu_blit_modex(void) {
    const uint8_t *src = (const uint8_t *)BACKBUF;
    uint32_t *fb = (uint32_t *)MODEX_FB;

    int w = SCREEN_WIDTH;
    int h = SCREEN_HEIGHT;
    int bytes_per_line = w >> 3;
    int dwords_per_line = bytes_per_line >> 2;

    for (int plane = 0; plane < 4; plane++) {
        outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
        outb(VGA_SEQ_DATA, (uint8_t)(1u << plane));

        for (int y = 0; y < h; y++) {
            const uint8_t *row = src + y * w;
            uint32_t *dst = fb + y * dwords_per_line;

            for (int bx = 0; bx < dwords_per_line; bx++) {
                const uint8_t *px = row + (bx << 5);
                uint32_t out0 = 0, out1 = 0, out2 = 0, out3 = 0;

                #define PACK(o, base) \
                    o  = ((uint32_t)(((px[base+0]>>plane)&1u)<<7)); \
                    o |= ((uint32_t)(((px[base+1]>>plane)&1u)<<6)); \
                    o |= ((uint32_t)(((px[base+2]>>plane)&1u)<<5)); \
                    o |= ((uint32_t)(((px[base+3]>>plane)&1u)<<4)); \
                    o |= ((uint32_t)(((px[base+4]>>plane)&1u)<<3)); \
                    o |= ((uint32_t)(((px[base+5]>>plane)&1u)<<2)); \
                    o |= ((uint32_t)(((px[base+6]>>plane)&1u)<<1)); \
                    o |= ((uint32_t)(((px[base+7]>>plane)&1u)<<0));

                PACK(out0,  0)
                PACK(out1,  8)
                PACK(out2, 16)
                PACK(out3, 24)
                #undef PACK

                dst[bx] = (out3 << 24) | (out2 << 16) | (out1 << 8) | out0;
            }
        }
    }

    outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
    outb(VGA_SEQ_DATA, 0x0F);
}

static inline void wait_vblank(void) {
    while ( (inb(0x3DA) & 0x08) == 0);
}

void gpu_blit(void) {
    if (g_video_mode == 0)
        gpu_blit_mode_13h();
    else {
        wait_vblank();
        gpu_blit_modex();
    }
}

gpu_backend_t gpu_backend(void) { return GPU_BACKEND_MODEX; }
