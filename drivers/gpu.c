#include "drivers/gpu.h"
#include "config/config.h"
#include "lib/core.h"
#include "lib/memory.h"

extern int g_video_mode;

#define MODEX_FB 0x000A0000U
#define VGA_SEQ_ADDR 0x03C4
#define VGA_SEQ_DATA 0x03C5
#define SEQ_MAP_MASK 0x02

/* ── EGA/VGA 16-colour planar blit ──────────────────────────────────────────
 *
 * Hardware layout (640x400x16c, BIOS mode 0x12 + CRTC patch):
 *   - 4 planes, each holds 1 bit per pixel
 *   - 640 pixels / 8 = 80 bytes per scanline per plane
 *   - Pixel colour index (0-15) = { plane3_bit, plane2_bit, plane1_bit,
 * plane0_bit }
 *
 * Backbuffer layout (software):
 *   - Linear array: 1 byte per pixel, values 0-15 (upper nibble ignored)
 *   - Width x Height bytes total (640 x 400 = 256 000 bytes)
 *
 * Blit strategy - write-mode 0, one plane at a time:
 *   For each plane p (0-3):
 *     Select only plane p via SEQ Map Mask register.
 *     For each scanline y, walk 8 pixels at a time.
 *     For each group of 8 horizontally adjacent pixels (x .. x+7):
 *       Build one output byte: bit 7 = pixel at x+0, bit 0 = pixel at x+7.
 *       Bit n of the output byte = (backbuffer[y*w + x + (7-n)] >> p) & 1
 *     Write that byte to fb[y * 80 + x/8].
 *
 * This produces exactly 80 bytes/line per plane with no splitting or doubling.
 */

uint32_t g_vesa_fb = MODEX_FB;

void gpu_init(void) {
    g_vesa_fb = MODEX_FB;
    /* Palette is already loaded correctly by BIOS mode 0x12.
     * gpu_init() is called after the mode is set, so nothing extra needed. */
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
    uint8_t *fb = (uint8_t *)MODEX_FB;

    int w = SCREEN_WIDTH;        /* 640 */
    int h = SCREEN_HEIGHT;       /* 400 */
    int bytes_per_line = w >> 3; /* 640/8 = 80 bytes per plane per scanline */

    for (int plane = 0; plane < 4; plane++) {
        /* Select only this plane for writing */
        outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
        outb(VGA_SEQ_DATA, (uint8_t)(1u << plane));

        for (int y = 0; y < h; y++) {
            const uint8_t *row = src + y * w;
            uint8_t *dst = fb + y * bytes_per_line;

            for (int bx = 0; bx < bytes_per_line; bx++) {
                /* Pack 8 consecutive pixels into one plane byte.
                 * MSB = leftmost pixel (x = bx*8), LSB = rightmost (x =
                 * bx*8+7). */
                const uint8_t *px = row + (bx << 3);
                uint8_t out = 0;
                out |= (uint8_t)(((px[0] >> plane) & 1u) << 7);
                out |= (uint8_t)(((px[1] >> plane) & 1u) << 6);
                out |= (uint8_t)(((px[2] >> plane) & 1u) << 5);
                out |= (uint8_t)(((px[3] >> plane) & 1u) << 4);
                out |= (uint8_t)(((px[4] >> plane) & 1u) << 3);
                out |= (uint8_t)(((px[5] >> plane) & 1u) << 2);
                out |= (uint8_t)(((px[6] >> plane) & 1u) << 1);
                out |= (uint8_t)(((px[7] >> plane) & 1u) << 0);
                dst[bx] = out;
            }
        }
    }

    /* Restore Map Mask to all planes (safe default) */
    outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
    outb(VGA_SEQ_DATA, 0x0F);
}

void gpu_blit(void) {
    if (g_video_mode == 0)
        gpu_blit_mode_13h();
    else
        gpu_blit_modex();
}

gpu_backend_t gpu_backend(void) { return GPU_BACKEND_MODEX; }
