#include "drivers/gpu.h"
#include "config/config.h"
#include "lib/core.h"
#include "lib/memory.h"

extern int g_video_mode;

#define MODEX_FB       0x000A0000U
#define VGA_SEQ_ADDR   0x03C4
#define VGA_SEQ_DATA   0x03C5
#define SEQ_MAP_MASK   0x02

static const uint8_t s_cga_pal[16][3] = {
    {0,  0,  0 }, {0,  0,  42}, {0,  42, 0 }, {0,  42, 42},
    {42, 0,  0 }, {42, 0,  42}, {42, 21, 0 }, {42, 42, 42},
    {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
    {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63},
};

static void dac_set_palette(void) {
    outb(0x3C8, 0);
    for (int i = 0; i < 256; i++) {
        if (i < 16) {
            outb(0x3C9, s_cga_pal[i][0]);
            outb(0x3C9, s_cga_pal[i][1]);
            outb(0x3C9, s_cga_pal[i][2]);
        } else {
            outb(0x3C9, 0);
            outb(0x3C9, 0);
            outb(0x3C9, 0);
        }
    }
}

uint32_t g_vesa_fb = MODEX_FB;

void gpu_init(void) {
    g_vesa_fb = MODEX_FB;
    dac_set_palette();
}

static void gpu_blit_mode_13h(void) {
    const uint8_t *src = (const uint8_t *)BACKBUF;
    uint8_t *fb = (uint8_t *)MODEX_FB;
    
    int w = SCREEN_WIDTH;
    int h = SCREEN_HEIGHT;
    
    int size = w * h;
    for (int i = 0; i < size; i++) {
        fb[i] = src[i];
    }
}

static void gpu_blit_modex(void) {
    const uint8_t *src = (const uint8_t *)BACKBUF;
    uint8_t *fb = (uint8_t *)MODEX_FB;

    int w = SCREEN_WIDTH;
    int h = SCREEN_HEIGHT;
    int plane_stride = w / 4;

    for (int plane = 0; plane < 4; plane++) {
        outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
        outb(VGA_SEQ_DATA, (uint8_t)(1 << plane));

        for (int y = 0; y < h; y++) {
            const uint8_t *row = src + y * w + plane;
            uint8_t *dst = fb + y * plane_stride;
            for (int x = plane; x < w; x += 4, dst++) {
                *dst = *row;
                row += 4;
            }
        }
    }

    outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
    outb(VGA_SEQ_DATA, 0x0F);
}

void gpu_blit(void) {
    if (g_video_mode == 0) {
        gpu_blit_mode_13h();
    } else {
        gpu_blit_modex();
    }
}

gpu_backend_t gpu_backend(void) { return GPU_BACKEND_MODEX; }
