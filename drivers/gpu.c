#include "drivers/gpu.h"
#include "config/config.h"
#include "drivers/pointer.h"
#include "lib/core.h"
#include "lib/io.h"
#include "lib/memory.h"
#include "lib/time.h"

/*
 * Back buffer (1 byte per pixel at BACKBUF) -> VGA.
 *
 * Only what changed since the last frame is sent: a shadow copy of the
 * last frame is kept and compared in 32-pixel blocks. On ISA VGA (1-3 MB/s)
 * writing video memory is the bottleneck, so an idle desktop now costs
 * almost nothing and a moving cursor only a few blocks.
 *
 * 640x400x16 is planar: 4 bit planes, 1 bit per pixel. A lookup table
 * spreads one pixel's 4 colour bits into the 4 plane bytes at once, so 8
 * pixels take 8 lookups instead of 32 bit extractions per plane.
 */

#define MODEX_FB 0x000A0000U
#define VGA_SEQ_ADDR 0x03C4
#define VGA_SEQ_DATA 0x03C5
#define SEQ_MAP_MASK 0x02

uint32_t g_vesa_fb = MODEX_FB;

static uint8_t *s_shadow;     /* last frame sent, NULL = always full */
static bool s_full = true;    /* next blit sends everything */
static uint32_t s_spread[16]; /* colour bit p -> bit 8*p */
static uint32_t s_frames, s_fps, s_fps_t0;
static uint32_t s_last_blocks; /* 32-pixel blocks sent by the last blit */

void gpu_init(void) {
    g_vesa_fb = MODEX_FB;
    for (int c = 0; c < 16; c++)
        s_spread[c] = (uint32_t)(c & 1) | ((uint32_t)((c >> 1) & 1) << 8) |
                      ((uint32_t)((c >> 2) & 1) << 16) |
                      ((uint32_t)((c >> 3) & 1) << 24);
    if (!s_shadow) /* only as big as this screen mode needs */
        s_shadow = (uint8_t *)kmalloc((uint32_t)(SCREEN_WIDTH * SCREEN_HEIGHT));
    s_full = true;
}

void gpu_invalidate(void) { s_full = true; }

/* 8 pixels -> plane bytes: byte p of the result is plane p (bit 7 = the
 * leftmost pixel). */
static inline uint32_t pack8(const uint8_t *px) {
    return (s_spread[px[0] & 15] << 7) | (s_spread[px[1] & 15] << 6) |
           (s_spread[px[2] & 15] << 5) | (s_spread[px[3] & 15] << 4) |
           (s_spread[px[4] & 15] << 3) | (s_spread[px[5] & 15] << 2) |
           (s_spread[px[6] & 15] << 1) | s_spread[px[7] & 15];
}

static inline bool same32(const uint8_t *a, const uint8_t *b) {
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3] &&
           x[4] == y[4] && x[5] == y[5] && x[6] == y[6] && x[7] == y[7];
}

static int s_y0, s_y1 = 399; /* rows to look at in this blit */

static void blit_planar(void) {
    const int w = SCREEN_WIDTH, h = SCREEN_HEIGHT;
    const int blocks = w / 32; /* 32 pixels = 1 dword/plane */
    uint32_t *fb = (uint32_t *)MODEX_FB;
    bool full = s_full || !s_shadow;
    uint32_t planes[4][20]; /* one row, <= 640 px */
    uint8_t dirty[20];
    uint32_t sent = 0;

    (void)h;
    for (int y = s_y0; y <= s_y1; y++) {
        const uint8_t *row = BACKBUF + y * w;
        uint8_t *shadow = s_shadow ? s_shadow + y * w : NULL;
        int n = 0;
        if (!full &&
            mem_same_run((const uint32_t *)row, (const uint32_t *)shadow,
                         (uint32_t)w / 4) == (uint32_t)w / 4)
            continue; /* whole row unchanged: one repe cmpsd */
        for (int b = 0; b < blocks; b++) {
            const uint8_t *px = row + b * 32;
            if (!full && same32(px, shadow + b * 32))
                continue;
            uint32_t g0 = pack8(px), g1 = pack8(px + 8), g2 = pack8(px + 16),
                     g3 = pack8(px + 24);
            for (int p = 0; p < 4; p++) {
                int sh = p * 8;
                planes[p][n] =
                    ((g0 >> sh) & 0xFF) | (((g1 >> sh) & 0xFF) << 8) |
                    (((g2 >> sh) & 0xFF) << 16) | (((g3 >> sh) & 0xFF) << 24);
            }
            dirty[n++] = (uint8_t)b;
            if (shadow)
                memcpy(shadow + b * 32, px, 32);
        }
        if (!n)
            continue;
        sent += (uint32_t)n;
        uint32_t *dst = fb + y * blocks;
        for (int p = 0; p < 4; p++) {
            outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
            outb(VGA_SEQ_DATA, (uint8_t)(1u << p));
            for (int i = 0; i < n; i++)
                dst[dirty[i]] = planes[p][i];
        }
    }
    outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
    outb(VGA_SEQ_DATA, 0x0F);
    s_last_blocks = sent;
}

/* 320x200x256 linear: send the runs of changed dwords. `repe cmpsd` skips
 * an unchanged run in one instruction (the old C loop compared dword by
 * dword); each changed run then goes out with rep movsd. */
static void blit_linear(void) {
    uint32_t n = (uint32_t)(SCREEN_WIDTH * SCREEN_HEIGHT) / 4;
    const uint32_t *src = (const uint32_t *)BACKBUF;
    uint32_t *fb = (uint32_t *)MODEX_FB;
    uint32_t *sh = (uint32_t *)s_shadow;
    uint32_t sent = 0;
    /* the shadow first, then the screen: the pointer (drivers/pointer.c)
     * may be put back from the shadow in between, and must not bring
     * back the old picture */
    if (s_full || !sh) {
        if (sh)
            memcpy(sh, src, n * 4);
        memcpy(fb, src, n * 4);
        s_last_blocks = n / 8;
        return;
    }
    uint32_t i = (uint32_t)(s_y0 * SCREEN_WIDTH) / 4;
    n = (uint32_t)((s_y1 + 1) * SCREEN_WIDTH) / 4;
    while (i < n) {
        i += mem_same_run(src + i, sh + i, n - i);
        if (i >= n)
            break;
        uint32_t run = mem_diff_run(src + i, sh + i, n - i);
        memcpy(sh + i, src + i, run * 4);
        memcpy(fb + i, src + i, run * 4);
        sent += run;
        i += run;
    }
    s_last_blocks = sent / 8; /* report in 32-pixel units like planar */
}

static void count_frame(void);

void gpu_blit(void) { gpu_blit_rows(0, SCREEN_HEIGHT - 1); }

/* Nothing was drawn this frame: the screen is already right. */
void gpu_blit_skip(void) {
    if (s_full) { /* someone asked for a full refresh meanwhile */
        gpu_blit();
        return;
    }
    s_last_blocks = 0;
    count_frame();
}

/* Only rows y0..y1 of the back buffer changed since the last blit. */
void gpu_blit_rows(int y0, int y1) {
    if (s_full || !s_shadow || y0 < 0)
        y0 = 0;
    if (s_full || !s_shadow || y1 >= SCREEN_HEIGHT)
        y1 = SCREEN_HEIGHT - 1;
    if (y0 > y1) {
        gpu_blit_skip();
        return;
    }
    s_y0 = y0;
    s_y1 = y1;
    if (g_video_mode == 0)
        blit_linear();
    else
        blit_planar();
    s_full = false;
    pointer_after_blit(y0, y1); /* the pointer stays on top */
    count_frame();
}

static void count_frame(void) {

    s_frames++;
    uint32_t now = time_millis();
    if (now - s_fps_t0 >= 1000) {
        s_fps = s_frames * 1000 / (now - s_fps_t0);
        s_frames = 0;
        s_fps_t0 = now;
    }
}

uint32_t gpu_fps(void) { return s_fps; }
uint32_t gpu_last_blocks(void) { return s_last_blocks; }

gpu_backend_t gpu_backend(void) { return GPU_BACKEND_MODEX; }

/* ── straight to the screen (the pointer) ──────────────────────────────── */

#define VGA_SEQ_ADDR_R 0x03C4

/* the rectangle of the last frame (shadow), with pix on top when given,
 * sent to the screen. Planar: whole 32-pixel blocks are rebuilt. */
void gpu_put(int x, int y, int w, int h, const uint8_t *pix) {
    if (!s_shadow)
        return;
    const int sw = SCREEN_WIDTH, shh = SCREEN_HEIGHT;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > sw ? sw : x + w, y1 = y + h > shh ? shh : y + h;
    if (x0 >= x1 || y0 >= y1)
        return;
    uint8_t old_idx = inb(VGA_SEQ_ADDR_R); /* an interrupted blit may be */
    outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);      /* half way through a plane   */
    uint8_t old_mask = inb(VGA_SEQ_DATA);

    if (g_video_mode == 0) { /* linear 256-colour */
        uint8_t *fb = (uint8_t *)MODEX_FB;
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) {
                uint8_t c = s_shadow[yy * sw + xx];
                if (pix && pix[(yy - y) * w + (xx - x)] != 0xFF)
                    c = pix[(yy - y) * w + (xx - x)];
                fb[yy * sw + xx] = c;
            }
    } else {
        uint32_t *fb = (uint32_t *)MODEX_FB;
        const int blocks = sw / 32;
        for (int yy = y0; yy < y1; yy++) {
            for (int b = x0 / 32; b <= (x1 - 1) / 32; b++) {
                uint8_t px[32];
                memcpy(px, s_shadow + yy * sw + b * 32, 32);
                if (pix)
                    for (int i = 0; i < 32; i++) {
                        int xx = b * 32 + i;
                        if (xx >= x0 && xx < x1 &&
                            pix[(yy - y) * w + (xx - x)] != 0xFF)
                            px[i] = pix[(yy - y) * w + (xx - x)];
                    }
                uint32_t g0 = pack8(px), g1 = pack8(px + 8),
                         g2 = pack8(px + 16), g3 = pack8(px + 24);
                for (int p = 0; p < 4; p++) {
                    int sh = p * 8;
                    outb(VGA_SEQ_DATA, (uint8_t)(1u << p));
                    fb[yy * blocks + b] = ((g0 >> sh) & 0xFF) |
                                          (((g1 >> sh) & 0xFF) << 8) |
                                          (((g2 >> sh) & 0xFF) << 16) |
                                          (((g3 >> sh) & 0xFF) << 24);
                }
            }
        }
    }
    outb(VGA_SEQ_DATA, old_mask);
    outb(VGA_SEQ_ADDR, old_idx);
}

uint32_t gpu_lock(void) {
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f)::"memory");
    return f;
}

void gpu_unlock(uint32_t f) {
    __asm__ volatile("pushl %0; popfl" ::"r"(f) : "memory", "cc");
}
