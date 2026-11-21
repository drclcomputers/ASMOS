/* Exercises the kernel's planar blit and gpu_put in every mode, with and
 * without 2x zoom, against a four-plane VGA model. run.sh builds and runs it.
 * The model maps VGA memory at 0xA0000 and selects planes by map mask. */
#include "lib/core.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "sys.h"

int g_screen_width, g_screen_height, g_backbuf_size, g_video_mode;
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

void outb(unsigned short p, unsigned char v) {
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

int sim_main(void) {
    fb = (uint8_t *)sys6(SYS_mmap2, 0xA0000, 65536, 3, 0x32, -1, 0);
    if (fb != (uint8_t *)0xA0000) {
        say("cannot map 0xA0000\n");
        return 1;
    }
    sys6(SYS_mmap2, 0x100000, 800 * 600, 3, 0x32, -1, 0); /* the back buffer */
    int fails = 0;
    static const int modes[][3] = {{1, 640, 400}, {2, 640, 480}, {3, 800, 600}};
    for (int m = 0; m < 3; m++)
        for (int z = 1; z <= 2; z++) {
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
    say(fails ? "SOME FAILED\n" : "all ok\n");
    return fails;
}
