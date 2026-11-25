/* Exercises the kernel's blits and gpu_put in every mode, with and without
 * 2x zoom: the planar ones against a four-plane VGA model (VGA memory at
 * 0xA0000, planes selected by map mask), the 256-colour ones against video
 * memory reached through mode 13h, a linear frame buffer, or a window the
 * "BIOS" moves (VESA 4F05h through realmode_int, here a model).
 * run.sh builds and runs it. */
#include "config/video_cfg.h"
#include "lib/core.h"
#include "lib/mem.h"
#include "lib/realmode.h"
#include "lib/string.h"
#include "sys.h"

int g_screen_width, g_screen_height, g_backbuf_size, g_video_mode;
vbe_info_t g_vbe;

int video_mode_size(int m) { return m & 3; }
int video_mode_colors(int m) { /* (as config/video_cfg.c) */
    static const uint8_t k_256[] = {1, 0, 0, 0, 0, 1, 1, 1};
    return k_256[m & 7] ? 256 : 16;
}

/* the card's memory (1 MB); the linear buffer is mapped at LFB */
#define LFB 0x40000000u
static uint8_t vram[1 << 20];
static int32_t bank = -1; /* where the window is, in granularity steps */
static int bank_calls;
bool vmm_map_device(uint32_t a, uint32_t n) { return a && n; }
uint32_t gpu_lock(void) { return 0; } /* (no cli out here) */
void gpu_unlock(uint32_t f) { (void)f; }

/* 4F05h: the window's bytes to the card, the new place's to the window */
void realmode_int(uint8_t n, rm_regs_t *r) {
    if (n != 0x10 || (r->eax & 0xFFFF) != 0x4F05 ||
        (r->ebx & 0xFF) != g_vbe.win)
        return;
    uint8_t *w = (uint8_t *)0xA0000;
    if (bank >= 0)
        memcpy(vram + bank * g_vbe.gran, w, g_vbe.win_size);
    bank = (int32_t)(r->edx & 0xFFFF);
    memcpy(w, vram + bank * g_vbe.gran, g_vbe.win_size);
    r->eax = 0x004F;
    bank_calls++;
}
extern uint8_t _backbuf[];
extern int g_zoom;
void gpu_init(void);
void gpu_set_zoom(int);
void gpu_blit(void);
void gpu_put(int, int, int, int, const uint8_t *);
void gpu_invalidate(void);

static uint8_t *fb;
static uint8_t planes[4][65536];
static int cur = -1;
static uint8_t seq_idx, seq_mask = 15;

static bool planar = true; /* the plane model on (16 colours) */

void outb(unsigned short p, unsigned char v) {
    if (!planar)
        return; /* (the DAC: not modelled) */
    if (p == 0x3C4)
        seq_idx = v;
    else if (p == 0x3C5 && seq_idx == 2) {
        if (cur >= 0)
            memcpy(planes[cur], fb, 65536);
        cur = v == 1 ? 0 : v == 2 ? 1 : v == 4 ? 2 : v == 8 ? 3 : -1;
        if (cur >= 0)
            memcpy(fb, planes[cur], 65536);
        seq_mask = v;
    }
}
unsigned char inb(unsigned short p) {
    return p == 0x3C5 ? seq_mask : p == 0x3C4 ? seq_idx : 0;
}
static uint8_t pool[1 << 20];
static uint32_t pool_used;
void *kmalloc(uint32_t n) {
    void *p = pool + pool_used;
    pool_used += (n + 15) & ~15u;
    return p;
}
uint32_t time_millis(void) { return 0; }
void pointer_after_blit(int a, int b) { (void)a, (void)b; }

static void say(const char *s) { sys3(SYS_write, 1, (int)s, (int)strlen(s)); }
static uint32_t seed = 1;
static int rnd(void) {
    return (int)((seed = seed * 1103515245u + 12345u) >> 16);
}

static int pixel(int x, int y, int pw) {
    int o = y * (pw / 8) + x / 8, bit = 7 - (x & 7), c = 0;
    for (int p = 0; p < 4; p++)
        c |= ((planes[p][o] >> bit) & 1) << p;
    return c;
}

/* the screen against the back buffer (each pixel g_zoom x g_zoom), with the
 * sprite on top where given */
static int check(const char *what, int pw, int ph, int sx, int sy,
                 const uint8_t *spr) {
    if (cur >= 0)
        memcpy(planes[cur], fb, 65536);
    int bad = 0, z = g_zoom, lw = g_screen_width;
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int lx = x / z, ly = y / z, want = _backbuf[ly * lw + lx] & 15;
            if (spr && lx >= sx && lx < sx + 9 && ly >= sy && ly < sy + 8 &&
                spr[(ly - sy) * 9 + (lx - sx)] != 0xFF)
                want = spr[(ly - sy) * 9 + (lx - sx)];
            bad += pixel(x, y, pw) != want;
        }
    char b[100];
    snprintf(b, sizeof(b), "  %-46s %s\n", what, bad ? "FAIL" : "ok");
    say(b);
    return bad != 0;
}

/* 256 colours: the screen's bytes against the back buffer */
static uint8_t px8(int x, int y, int pitch, int mode) {
    if (mode == 0)
        return ((uint8_t *)0xA0000)[y * pitch + x];
    if (g_vbe.lfb)
        return ((uint8_t *)LFB)[y * pitch + x];
    return vram[y * pitch + x];
}

static int check8(const char *what, int pw, int ph, int pitch, int mode, int sx,
                  int sy, const uint8_t *spr) {
    if (!g_vbe.lfb && mode != 0 && bank >= 0) /* the window back to vram */
        memcpy(vram + bank * g_vbe.gran, (uint8_t *)0xA0000, g_vbe.win_size);
    int bad = 0, z = g_zoom, lw = g_screen_width;
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int lx = x / z, ly = y / z, want = _backbuf[ly * lw + lx];
            if (spr && lx >= sx && lx < sx + 9 && ly >= sy && ly < sy + 8 &&
                spr[(ly - sy) * 9 + (lx - sx)] != 0xFF)
                want = spr[(ly - sy) * 9 + (lx - sx)];
            bad += px8(x, y, pitch, mode) != want;
        }
    char b[100];
    snprintf(b, sizeof(b), "  %-46s %s\n", what, bad ? "FAIL" : "ok");
    say(b);
    return bad != 0;
}

static int test256(void) {
    int fails = 0;
    static const struct {
        int mode, w, h, pitch;
        uint32_t lfb, gran;
        uint8_t win;
        const char *name;
    } cases[] = {
        {0, 320, 200, 320, 0, 0, 0, "320x200x256, mode 13h"},
        {6, 640, 480, 640, LFB, 0, 0, "640x480x256, linear"},
        {7, 800, 600, 1024, LFB, 0, 0, "800x600x256, linear, 1024 a line"},
        {6, 640, 480, 640, 0, 65536, 0, "640x480x256, window A, 64 KB steps"},
        {7, 800, 600, 832, 0, 4096, 1, "800x600x256, window B, 4 KB steps"},
        {5, 640, 400, 640, 0, 16384, 0, "640x400x256, window, 16 KB steps"},
    };
    planar = false;
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
        for (int z = 1; z <= 2; z++) {
            if (cases[c].mode == 0 && z == 2)
                continue; /* (no zoom at 320x200) */
            memset(&g_vbe, 0, sizeof(g_vbe));
            if (cases[c].mode) {
                g_vbe.mode = 0x101;
                g_vbe.pitch = (uint16_t)cases[c].pitch;
                g_vbe.lfb = cases[c].lfb;
                g_vbe.gran = cases[c].gran;
                g_vbe.win_size = 65536;
                g_vbe.win_addr = 0xA0000;
                g_vbe.win = cases[c].win;
            }
            memset(vram, 0, sizeof(vram));
            memset((void *)LFB, 0, 1 << 20);
            memset((void *)0xA0000, 0, 65536);
            bank = -1;
            bank_calls = 0;
            g_video_mode = cases[c].mode;
            g_screen_width = cases[c].w;
            g_screen_height = cases[c].h;
            g_zoom = 1;
            g_backbuf_size = g_screen_width * g_screen_height;
            gpu_init();
            gpu_set_zoom(z);
            gpu_invalidate();
            int pw = cases[c].w, ph = cases[c].h, lw = g_screen_width,
                lh = g_screen_height, pitch = cases[c].pitch;
            char b[100];
            snprintf(b, sizeof(b), "%s, zoom %d:\n", cases[c].name, z);
            say(b);
            for (int i = 0; i < lw * lh; i++)
                _backbuf[i] = (uint8_t)rnd();
            gpu_blit();
            fails += check8("a whole frame", pw, ph, pitch, cases[c].mode, 0, 0,
                            NULL);
            for (int k = 0; k < 300; k++)
                _backbuf[(rnd() % lh) * lw + rnd() % lw] = (uint8_t)rnd();
            int before = bank_calls;
            gpu_blit();
            fails += check8("only what changed", pw, ph, pitch, cases[c].mode,
                            0, 0, NULL);
            uint8_t spr[9 * 8];
            for (int i = 0; i < 72; i++)
                spr[i] = (i % 3) ? (uint8_t)(i * 7) : 0xFF;
            int32_t was = bank;
            gpu_put(lw - 5, lh - 3, 9, 8, spr);
            fails += check8("the pointer, off the bottom right", pw, ph, pitch,
                            cases[c].mode, lw - 5, lh - 3, spr);
            if (g_vbe.gran && !g_vbe.lfb && bank != was) {
                say("  the window not put back after the pointer FAIL\n");
                fails++;
            }
            gpu_put(lw - 5, lh - 3, 9, 8, NULL);
            fails += check8("the pointer taken away", pw, ph, pitch,
                            cases[c].mode, 0, 0, NULL);
            if (g_vbe.gran && !g_vbe.lfb) {
                snprintf(b, sizeof(b), "  (%d window moves for 300 pixels)\n",
                         bank_calls - before);
                say(b);
            }
        }
    planar = true;
    return fails;
}

int sim_main(void) {
    fb = (uint8_t *)sys6(SYS_mmap2, 0xA0000, 65536, 3, 0x32, -1, 0);
    if (fb != (uint8_t *)0xA0000) {
        say("cannot map 0xA0000\n");
        return 1;
    }
    sys6(SYS_mmap2, 0x100000, 800 * 600, 3, 0x32, -1, 0); /* the back buffer */
    sys6(SYS_mmap2, LFB, 1 << 20, 3, 0x32, -1, 0);        /* the LFB */
    int fails = 0;
    static const int modes[][3] = {
        {1, 640, 400}, {2, 640, 480}, {3, 800, 600}, {4, 320, 200}};
    for (int m = 0; m < 4; m++)
        for (int z = 1; z <= (modes[m][1] >= 640 ? 2 : 1); z++) {
            g_video_mode = modes[m][0];
            g_screen_width = modes[m][1];
            g_screen_height = modes[m][2];
            g_zoom = 1;
            g_backbuf_size = g_screen_width * g_screen_height;
            memset(planes, 0, sizeof(planes));
            cur = -1;
            gpu_init();
            gpu_set_zoom(z);
            gpu_invalidate();
            int pw = modes[m][1], ph = modes[m][2];
            int lw = g_screen_width, lh = g_screen_height;
            char b[80];
            snprintf(b, sizeof(b), "%dx%d, zoom %d (%dx%d):\n", pw, ph, z, lw,
                     lh);
            say(b);
            for (int i = 0; i < lw * lh; i++)
                _backbuf[i] = (uint8_t)(rnd() & 15);
            gpu_blit();
            fails += check("a whole frame", pw, ph, 0, 0, NULL);
            for (int k = 0; k < 300; k++)
                _backbuf[(rnd() % lh) * lw + rnd() % lw] =
                    (uint8_t)(rnd() & 15);
            gpu_blit();
            fails += check("only what changed", pw, ph, 0, 0, NULL);
            uint8_t spr[9 * 8];
            for (int i = 0; i < 72; i++)
                spr[i] = (i % 3) ? (uint8_t)(i & 15) : 0xFF;
            gpu_put(lw - 5, 37, 9, 8, spr);
            fails += check("the pointer, half off the right edge", pw, ph,
                           lw - 5, 37, spr);
            gpu_put(lw - 5, 37, 9, 8, NULL);
            fails += check("the pointer taken away", pw, ph, 0, 0, NULL);
        }
    fails += test256();
    say(fails ? "SOME FAILED\n" : "all ok\n");
    return fails;
}
