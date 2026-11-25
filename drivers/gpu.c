// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/gpu.h"
#include "config/config.h"
#include "config/video_cfg.h"
#include "drivers/pointer.h"
#include "lib/core.h"
#include "lib/io.h"
#include "lib/memory.h"
#include "lib/realmode.h"
#include "lib/time.h"
#include "lib/vmm.h"

/*
 * Copies the 1-byte-per-pixel back buffer to the screen. A shadow frame
 * detects changes, reducing writes on slow ISA video. The 16-color modes use
 * four 1-bit planes; a lookup table converts eight pixels to plane bytes in
 * eight lookups. The 256-colour ones are a byte a pixel: mode 13h at
 * 0xA0000, a VESA mode's linear frame buffer, or its 64 KB window moved
 * along by the video BIOS (VESA 4F05h, through lib/realmode.h).
 */

#define MODEX_FB 0x000A0000U
#define VGA_SEQ_ADDR 0x03C4
#define VGA_SEQ_DATA 0x03C5
#define SEQ_MAP_MASK 0x02

uint32_t g_vesa_fb = MODEX_FB;

/* 256 colours: where the pixels go */
static uint8_t *s_fb8;         /* the screen's first byte (or the window) */
static uint32_t s_pitch;       /* bytes a screen line */
static bool s_banked;          /* through a window */
static uint32_t s_gran, s_win; /* the window: its steps, its size */
static int32_t s_bank = -1;    /* where it is now (in steps); -1 = unknown */

static bool is8(void) { return video_mode_colors(g_video_mode) == 256; }

static uint8_t *s_shadow;     /* last frame sent, NULL = always full */
static bool s_full = true;    /* next blit sends everything */
static uint32_t s_spread[16]; /* colour bit p -> bit 8*p */
static uint32_t s_frames, s_fps, s_fps_t0;
static uint32_t s_last_blocks; /* 32-pixel blocks sent by the last blit */

/* The 16 colours, set by us rather than left to the BIOS: some video BIOSes
 * set a planar mode without its palette (QEMU's for VESA 800x600 leaves the
 * screen black). Attribute controller: colour i -> DAC entry i; DAC: the
 * usual EGA/VGA colours (6 bits each), the same ones mode 13h starts with. */
static const uint8_t k_dac16[16][3] = {
    {0, 0, 0},    {0, 0, 42},   {0, 42, 0},   {0, 42, 42},
    {42, 0, 0},   {42, 0, 42},  {42, 21, 0},  {42, 42, 42},
    {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
    {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63}};

/* 256 colours: the BIOS's mode 13h palette (6 bits), set in every 256-colour
 * mode alike: the 16 colours, 16 greys, then 9 rings of 24 hues (3
 * brightnesses x 3 saturations), black */
static uint8_t s_pal256[256][3];

static void make_pal256(void) {
    static const uint8_t grey[16] = {0,  5,  8,  11, 14, 17, 20, 24,
                                     28, 32, 36, 40, 45, 50, 56, 63};
    static const uint8_t ring[9][2] = {{63, 0}, {63, 31}, {63, 45},
                                       {28, 0}, {28, 14}, {28, 20},
                                       {16, 0}, {16, 8},  {16, 11}};
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 3; k++) {
            s_pal256[i][k] = k_dac16[i][k];
            s_pal256[16 + i][k] = grey[i];
        }
    for (int r = 0; r < 9; r++) {
        int hi = ring[r][0], lo = ring[r][1], st[5];
        for (int k = 0; k < 5; k++)
            st[k] = lo + (hi - lo) * k / 4;
        /* blue -> magenta -> red -> yellow -> green -> cyan -> blue */
        for (int h = 0; h < 24; h++) {
            int seg = h / 4, k = h % 4, c[3];
            static const int8_t shape[6][3] = {{1, -1, 2}, {2, -1, -2},
                                               {2, 1, -1}, {-2, 2, -1},
                                               {-1, 2, 1}, {-1, -2, 2}};
            for (int j = 0; j < 3; j++) { /* 2 hi, -1 lo, 1 up, -2 down */
                int v = shape[seg][j];
                c[j] = v == 2 ? hi : v == -1 ? lo : v == 1 ? st[k] : st[4 - k];
            }
            for (int j = 0; j < 3; j++)
                s_pal256[32 + r * 24 + h][j] = (uint8_t)c[j];
        }
    }
    /* 248..255 stay black */
}

static void palette256(void) {
    outb(0x3C8, 0);
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 3; k++)
            outb(0x3C9, s_pal256[i][k]);
}

static void planar_palette(void) {
    (void)inb(0x3DA); /* the attribute controller: back to "index" */
    for (uint8_t i = 0; i < 16; i++) {
        outb(0x3C0, i);
        outb(0x3C0, i);
    }
    static const uint8_t ac[][2] = {{0x10, 0x01}, /* graphics, no blink */
                                    {0x11, 0x00}, /* border black */
                                    {0x12, 0x0F}, /* all 4 planes shown */
                                    {0x13, 0x00}, /* no panning */
                                    {0x14, 0x00}};
    for (unsigned i = 0; i < sizeof(ac) / sizeof(ac[0]); i++) {
        outb(0x3C0, ac[i][0]);
        outb(0x3C0, ac[i][1]);
    }
    outb(0x3C0, 0x20); /* palette set: the screen back on */
    outb(0x3C8, 0);
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 3; k++)
            outb(0x3C9, k_dac16[i][k]);
}

void gpu_init(void) {
    g_vesa_fb = MODEX_FB;
    make_pal256();
    s_bank = -1;
    if (is8()) {
        int pw = SCREEN_WIDTH * g_zoom;
        s_fb8 = (uint8_t *)MODEX_FB;
        s_pitch = (uint32_t)pw;
        s_banked = false;
        if (g_vbe.mode) { /* a VESA mode */
            s_pitch = g_vbe.pitch ? g_vbe.pitch : (uint32_t)pw;
            if (g_vbe.lfb) {
                s_fb8 = (uint8_t *)(uintptr_t)g_vbe.lfb;
            } else {
                s_banked = true;
                s_win = g_vbe.win_size ? g_vbe.win_size : 65536;
                s_gran = g_vbe.gran && g_vbe.gran <= s_win ? g_vbe.gran : s_win;
                s_fb8 = (uint8_t *)(uintptr_t)(g_vbe.win_addr ? g_vbe.win_addr
                                                              : MODEX_FB);
            }
        }
        palette256();
    } else {
        planar_palette();
    }
    for (int c = 0; c < 16; c++)
        s_spread[c] = (uint32_t)(c & 1) | ((uint32_t)((c >> 1) & 1) << 8) |
                      ((uint32_t)((c >> 2) & 1) << 16) |
                      ((uint32_t)((c >> 3) & 1) << 24);
    if (!s_shadow) /* only as big as this screen mode needs */
        s_shadow = (uint8_t *)kmalloc((uint32_t)(SCREEN_WIDTH * SCREEN_HEIGHT));
    s_full = true;
}

void gpu_invalidate(void) { s_full = true; }

/* a linear frame buffer sits past the RAM: in the page tables (kmain, once
 * paging is on, before any app context copies them) */
void gpu_map(void) {
    if (is8() && g_vbe.mode && g_vbe.lfb)
        vmm_map_device(g_vbe.lfb, s_pitch * (uint32_t)(SCREEN_HEIGHT * g_zoom));
}

int gpu_palette(uint8_t (*rgb)[3], int max) {
    int n = is8() ? 256 : 16;
    for (int i = 0; i < n && i < max; i++)
        for (int k = 0; k < 3; k++)
            rgb[i][k] = s_pal256[i][k]; /* (the first 16: the same) */
    return n;
}

// 256 colours: bytes to the screen

/* the window to `pos` (in steps), by the video BIOS */
static void set_bank(int32_t pos) {
    uint32_t f = gpu_lock(); /* (the pointer's interrupt puts it back) */
    rm_regs_t r = {0};
    r.eax = 0x4F05;
    r.ebx = g_vbe.win; /* BH 0: set */
    r.edx = (uint32_t)pos;
    realmode_int(0x10, &r);
    s_bank = pos;
    gpu_unlock(f);
}

/* n bytes at offset off of the screen */
static void put_bytes(uint32_t off, const uint8_t *src, uint32_t n) {
    if (!s_banked) {
        memcpy(s_fb8 + off, src, n);
        return;
    }
    while (n) {
        uint32_t start = (uint32_t)s_bank * s_gran;
        if (s_bank < 0 || off < start || off >= start + s_win) {
            set_bank((int32_t)(off / s_gran));
            start = (uint32_t)s_bank * s_gran;
        }
        uint32_t k = start + s_win - off;
        if (k > n)
            k = n;
        memcpy(s_fb8 + (off - start), src, k);
        off += k, src += k, n -= k;
    }
}

/* n logical pixels at (x, y): zoomed, each goes out as 2x2 */
static void put_run(int y, int x, const uint8_t *px, int n) {
    if (g_zoom == 1) {
        put_bytes((uint32_t)y * s_pitch + (uint32_t)x, px, (uint32_t)n);
        return;
    }
    uint8_t tmp[800];
    for (int k = 0; k < n; k++)
        tmp[2 * k] = tmp[2 * k + 1] = px[k];
    for (int r = 0; r < 2; r++)
        put_bytes((uint32_t)(2 * y + r) * s_pitch + 2u * (uint32_t)x, tmp,
                  2u * (uint32_t)n);
}

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

// 2x zoom (Settings > System, the planar modes)
/*
 * The back buffer, the shadow and every coordinate are in "logical" pixels
 * (half the screen each way); each one goes out as a 2x2 square. A screen
 * block of 32 pixels is then 16 logical ones. */
int g_zoom = 1;

void gpu_set_zoom(int z) {
    if (video_mode_size(g_video_mode) == 0 || (z != 1 && z != 2))
        z = 1; /* not 320x200 */
    if (z == g_zoom)
        return;
    int pw = SCREEN_WIDTH * g_zoom, ph = SCREEN_HEIGHT * g_zoom;
    g_zoom = z;
    g_screen_width = pw / z;
    g_screen_height = ph / z;
    g_backbuf_size = g_screen_width * g_screen_height;
    s_full = true;
}

/* 32 screen pixels from a block of logical ones */
static inline const uint8_t *screen_px(const uint8_t *lpx, uint8_t *tmp) {
    if (g_zoom == 1)
        return lpx;
    for (int i = 0; i < 16; i++)
        tmp[2 * i] = tmp[2 * i + 1] = lpx[i];
    return tmp;
}

static inline bool same_n(const uint8_t *a, const uint8_t *b, int n) {
    return n == 32 ? same32(a, b) : !memcmp(a, b, (uint32_t)n);
}

static void blit_planar(void) {
    const int w = SCREEN_WIDTH, z = g_zoom;
    const int lb = 32 / z;     /* logical pixels in a 32-pixel block */
    const int blocks = w / lb; /* 32 pixels = 1 dword/plane */
    uint32_t *fb = (uint32_t *)MODEX_FB;
    bool full = s_full || !s_shadow;
    uint32_t planes[4][25]; /* one row, <= 800 px */
    uint8_t dirty[25], tmp[32];
    uint32_t sent = 0;

    for (int y = s_y0; y <= s_y1; y++) {
        const uint8_t *row = BACKBUF + y * w;
        uint8_t *shadow = s_shadow ? s_shadow + y * w : NULL;
        int n = 0;
        if (!full &&
            mem_same_run((const uint32_t *)row, (const uint32_t *)shadow,
                         (uint32_t)w / 4) == (uint32_t)w / 4)
            continue; /* whole row unchanged: one repe cmpsd */
        for (int b = 0; b < blocks; b++) {
            const uint8_t *lpx = row + b * lb;
            if (!full && same_n(lpx, shadow + b * lb, lb))
                continue;
            const uint8_t *px = screen_px(lpx, tmp);
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
                memcpy(shadow + b * lb, lpx, (uint32_t)lb);
        }
        if (!n)
            continue;
        sent += (uint32_t)n;
        for (int p = 0; p < 4; p++) {
            outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
            outb(VGA_SEQ_DATA, (uint8_t)(1u << p));
            for (int r = 0; r < z; r++) { /* zoomed: the row twice */
                uint32_t *dst = fb + (y * z + r) * blocks;
                for (int i = 0; i < n; i++)
                    dst[dirty[i]] = planes[p][i];
            }
        }
    }
    outb(VGA_SEQ_ADDR, SEQ_MAP_MASK);
    outb(VGA_SEQ_DATA, 0x0F);
    s_last_blocks = sent;
}

/* 256 colours: per line, the runs of changed dwords. `repe cmpsd` skips an
 * unchanged run in one instruction; each changed run goes out at once. */
static void blit8(void) {
    const int w = SCREEN_WIDTH;
    const uint32_t n = (uint32_t)w / 4;
    bool full = s_full || !s_shadow;
    uint32_t sent = 0;
    for (int y = s_y0; y <= s_y1; y++) {
        const uint32_t *src = (const uint32_t *)(BACKBUF + y * w);
        uint32_t *sh = s_shadow ? (uint32_t *)(s_shadow + y * w) : NULL;
        uint32_t i = 0;
        while (i < n) {
            uint32_t run = n - i;
            if (!full) {
                i += mem_same_run(src + i, sh + i, n - i);
                if (i >= n)
                    break;
                run = mem_diff_run(src + i, sh + i, n - i);
            }
            /* the shadow first, then the screen: the pointer (drivers/
             * pointer.c) may be put back from the shadow in between, and
             * must not bring back the old picture */
            if (sh)
                memcpy(sh + i, src + i, run * 4);
            put_run(y, (int)i * 4, (const uint8_t *)(src + i), (int)run * 4);
            sent += run;
            i += run;
        }
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
    if (is8())
        blit8();
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

gpu_backend_t gpu_backend(void) {
    if (!is8())
        return GPU_BACKEND_MODEX;
    return s_banked ? GPU_BACKEND_BANKED : GPU_BACKEND_LINEAR;
}

// straight to the screen (the pointer)

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

    if (is8()) { /* a byte a pixel; the window put back where it was */
        int32_t bank = s_bank;
        for (int yy = y0; yy < y1; yy++) {
            uint8_t line[800];
            for (int xx = x0; xx < x1; xx++) {
                uint8_t c = s_shadow[yy * sw + xx];
                if (pix && pix[(yy - y) * w + (xx - x)] != 0xFF)
                    c = pix[(yy - y) * w + (xx - x)];
                line[xx - x0] = c;
            }
            put_run(yy, x0, line, x1 - x0);
        }
        if (s_banked && bank >= 0 && s_bank != bank)
            set_bank(bank);
    } else {
        /* in logical pixels; zoomed, each goes out as 2x2 (blit_planar) */
        uint32_t *fb = (uint32_t *)MODEX_FB;
        const int z = g_zoom, lb = 32 / z, blocks = sw / lb;
        for (int yy = y0; yy < y1; yy++) {
            for (int b = x0 / lb; b <= (x1 - 1) / lb; b++) {
                uint8_t lpx[32], tmp[32];
                memcpy(lpx, s_shadow + yy * sw + b * lb, (uint32_t)lb);
                if (pix)
                    for (int i = 0; i < lb; i++) {
                        int xx = b * lb + i;
                        if (xx >= x0 && xx < x1 &&
                            pix[(yy - y) * w + (xx - x)] != 0xFF)
                            lpx[i] = pix[(yy - y) * w + (xx - x)];
                    }
                const uint8_t *px = screen_px(lpx, tmp);
                uint32_t g0 = pack8(px), g1 = pack8(px + 8),
                         g2 = pack8(px + 16), g3 = pack8(px + 24);
                for (int p = 0; p < 4; p++) {
                    int sh = p * 8;
                    uint32_t v = ((g0 >> sh) & 0xFF) |
                                 (((g1 >> sh) & 0xFF) << 8) |
                                 (((g2 >> sh) & 0xFF) << 16) |
                                 (((g3 >> sh) & 0xFF) << 24);
                    outb(VGA_SEQ_DATA, (uint8_t)(1u << p));
                    for (int r = 0; r < z; r++)
                        fb[(yy * z + r) * blocks + b] = v;
                }
            }
        }
    }
    outb(VGA_SEQ_DATA, old_mask);
    outb(VGA_SEQ_ADDR, old_idx);
}

/* (weak: tests/sim/gputest.c runs where cli isn't allowed) */
__attribute__((weak)) uint32_t gpu_lock(void) {
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f)::"memory");
    return f;
}

__attribute__((weak)) void gpu_unlock(uint32_t f) {
    __asm__ volatile("pushl %0; popfl" ::"r"(f) : "memory", "cc");
}
