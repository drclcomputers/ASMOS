#include "fs/install.h"
#include "drivers/ata.h"
#include "ui/popup.h"
/* ASMOS UI simulator: runs the real UI/app code as a 32-bit Linux process,
 * with hardware stubbed, scripted input and PPM screenshots. */
#include "sys.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/graphics.h"
#include "config/config.h"
#include "config/runtime_config.h"
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "fs/fs.h"
#include "os/os.h"
#include "os/app_registry.h"
#include "os/error.h"
#include "ui/ui.h"
#include "os/clipboard.h"

/* ── globals normally in kernel.c / idt.c / scheduler.c ── */
int g_screen_width = 640, g_screen_height = 400, g_video_mode = 1,
    g_backbuf_size = 640 * 400;
uint8_t _heap_start;
volatile uint32_t pit_ticks, pit_seconds;
mousevar mouse;


/* ── hardware stubs ── */
/* fake CMOS clock: BCD, 24 h, 2026-11-16 10:35:00 Monday */
static unsigned char s_cmos_idx;
unsigned char g_cmos[128] = {[0x00] = 0x00, [0x02] = 0x35, [0x04] = 0x10,
    [0x06] = 0x02, [0x07] = 0x16, [0x08] = 0x11, [0x09] = 0x26,
    [0x0B] = 0x02, [0x10] = 0x40 /* A: 1.44 MB */, [0x32] = 0x20};
/* fake printer on LPT1 (0x378): always ready; strobed bytes collected */
unsigned char g_lpt_out[262144];
unsigned g_lpt_n;
int g_lpt_status = 0xD8; /* nBUSY | nACK | SELECT | nERROR */
static unsigned char s_lpt_latch, s_lpt_ctl;
extern _Bool fdc_port_in(unsigned short p, unsigned char *v);
extern _Bool fdc_port_out(unsigned short p, unsigned char v);
unsigned char inb(unsigned short p) {
    unsigned char fv;
    if (fdc_port_in(p, &fv)) return fv;
    if (p == 0x71) return g_cmos[s_cmos_idx & 0x7F];
    if (p == 0x378) return s_lpt_latch;
    if (p == 0x379) return (unsigned char)g_lpt_status;
    return 0;
}
unsigned short inw(unsigned short p) { (void)p; return 0; }
uint32_t inl(unsigned short p) { (void)p; return 0; }
void outb(unsigned short p, unsigned char v) {
    if (fdc_port_out(p, v)) return;
    if (p == 0x378) s_lpt_latch = v;
    if (p == 0x37A) {
        if ((v & 1) && !(s_lpt_ctl & 1) && g_lpt_n < sizeof(g_lpt_out))
            g_lpt_out[g_lpt_n++] = s_lpt_latch; /* strobe */
        s_lpt_ctl = v;
    }
    if (p == 0x70) s_cmos_idx = v & 0x7F;
    else if (p == 0x71) g_cmos[s_cmos_idx & 0x7F] = v;
}
void outw(unsigned short p, unsigned short v) { (void)p; (void)v; }
void outl(unsigned short p, uint32_t v) { (void)p; (void)v; }
void cpu_halt(void) {}
void cpu_idle(void) {}
void cpu_shutdown(void) { sys3(SYS_exit, 0, 0, 0); }
void cpu_reset(void) { sys3(SYS_exit, 0, 0, 0); }
/* the screen, as the real driver keeps it (drivers/gpu.c, linear): what
 * was sent last (shadow); the pointer goes on top (drivers/pointer.c) */
unsigned char g_sim_vram[640 * 400], g_sim_shadow[640 * 400];
static int s_sfull = 1;
int g_sim_puts; /* pixels the pointer sent */
void gpu_put(int x, int y, int w, int h, const unsigned char *pix) {
    for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++) {
        if (xx < 0 || yy < 0 || xx >= SCREEN_WIDTH || yy >= SCREEN_HEIGHT) continue;
        unsigned char c = g_sim_shadow[yy * SCREEN_WIDTH + xx];
        if (pix && pix[(yy - y) * w + (xx - x)] != 0xFF) c = pix[(yy - y) * w + (xx - x)];
        g_sim_vram[yy * SCREEN_WIDTH + xx] = c; g_sim_puts++; }
}
uint32_t gpu_lock(void) { return 0; }
void gpu_unlock(uint32_t f) { (void)f; }
void pointer_after_blit(int, int);
void gpu_blit_rows(int a, int b) {
    if (s_sfull || a < 0) a = 0;
    if (s_sfull || b >= SCREEN_HEIGHT) b = SCREEN_HEIGHT - 1;
    for (int y = a; y <= b; y++) {
        memcpy(g_sim_shadow + y * SCREEN_WIDTH, BACKBUF + y * SCREEN_WIDTH, SCREEN_WIDTH);
        memcpy(g_sim_vram + y * SCREEN_WIDTH, BACKBUF + y * SCREEN_WIDTH, SCREEN_WIDTH); }
    s_sfull = 0;
    pointer_after_blit(a, b);
}
void gpu_blit(void) { gpu_blit_rows(0, SCREEN_HEIGHT - 1); }
void gpu_blit_skip(void) { if (s_sfull) gpu_blit(); }
void gpu_invalidate(void) { s_sfull = 1; }
/* no timer interrupt here: input is polled, the frame moves the pointer */
void ps2_irq_mode(bool on) { (void)on; }
bool ps2_irq_active(void) { return false; }
int ps2_key_byte(void) { return -1; }
void ps2_irq_poll(void) {}
const mousevar *mouse_live(void) { return &mouse; }
void gpu_init(void) {}

/* two IDE disks: HDA (the image) and HDB (absent unless a test adds it) */
static uint8_t *g_dk[2];
static uint32_t g_dk_sectors[2];
#define g_disk g_dk[0]
#define g_disk_sectors g_dk_sectors[0]
static ata_disk_t g_dk_info[2];
const char *g_sim_model_a = "SIM DISK A";
const ata_disk_t *ata_unit(uint8_t u) {
    if (u > 1 || !g_dk_sectors[u]) return NULL;
    g_dk_info[u].present = true;
    g_dk_info[u].lba = true;
    g_dk_info[u].sectors = g_dk_sectors[u];
    extern const char *g_sim_model_a;
    if (!g_dk_info[u].model[0]) strcpy(g_dk_info[u].model, u ? "SIM DISK B" : g_sim_model_a);
    return &g_dk_info[u];
}
int g_ata_writes, g_ata_reads;
bool ata_read_sector(uint8_t d, uint32_t lba, void *b) {
    if (d > 1 || lba >= g_dk_sectors[d]) return false;
    g_ata_reads++;
    memcpy(b, g_dk[d] + lba * 512, 512); return true;
}
bool ata_write_sector(uint8_t d, uint32_t lba, const void *b) {
    if (d > 1 || lba >= g_dk_sectors[d]) return false;
    g_ata_writes++;
    memcpy(g_dk[d] + lba * 512, b, 512); return true;
}
/* tests: a blank disk of n sectors (or none), a name */
void sim_disk_blank(int u, uint32_t n, const char *model) {
    memset(g_dk[u], 0, (size_t)g_dk_sectors[u] * 512 > (size_t)n * 512 ? (size_t)g_dk_sectors[u] * 512 : (size_t)n * 512);
    g_dk_sectors[u] = n;
    strcpy(g_dk_info[u].model, model ? model : "");
}
void sim_disk_save(int u, const char *path) {
    extern void sim_save(const char *, const void *, unsigned);
    sim_save(path, g_dk[u], g_dk_sectors[u] * 512);
}
int g_ata_parks; void ata_park(bool spin_down) { (void)spin_down; g_ata_parks++; }
bool ata_read_sectors(uint8_t d, uint32_t l, uint8_t c, void *b) {
    for (int i = 0; i < c; i++) if (!ata_read_sector(d, l + i, (uint8_t *)b + i * 512)) return false;
    return true;
}
bool ata_write_sectors(uint8_t d, uint32_t l, uint8_t c, const void *b) {
    for (int i = 0; i < c; i++) if (!ata_write_sector(d, l + i, (const uint8_t *)b + i * 512)) return false;
    return true;
}

/* ── scheduler stubs ── */
/* real cooperative tasks: simtask.c */

/* ── tiny libc for the harness ── */
void wr(int fd, const char *s) { sys3(SYS_write, fd, (int)s, strlen(s)); }
void sim_log(const char *s) { wr(1, s); wr(1, "\n"); }

static void *map(uint32_t addr, uint32_t len) {
    int flags = 0x02 | 0x20 | (addr ? 0x10 : 0); /* PRIVATE|ANON|FIXED */
    int r = sys6(SYS_mmap2, (int)addr, (int)len, 7, flags, -1, 0);
    if ((unsigned)r > 0xFFFFF000u) { wr(2, "mmap failed\n"); sys3(SYS_exit, 2, 0, 0); }
    return (void *)r;
}

bool load_file(const char *path, uint8_t *dst, uint32_t max, uint32_t *got) {
    int fd = sys3(SYS_open, (int)path, 0, 0);
    if (fd < 0) { wr(1, "missing: "); wr(1, path); wr(1, "\n"); return false; }
    uint32_t n = 0;
    for (;;) {
        int r = sys3(SYS_read, fd, (int)(dst + n), (int)(max - n > 65536 ? 65536 : max - n));
        if (r <= 0) break;
        n += r;
        if (n >= max) break;
    }
    sys3(SYS_close, fd, 0, 0);
    *got = n;
    return true;
}

/* ── VGA 16-colour palette ── */
static const uint8_t PAL[16][3] = {
    {0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},{170,85,0},{170,170,170},
    {85,85,85},{85,85,255},{85,255,85},{85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}};

void sim_shot(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "out/%s.ppm", name);
    int fd = sys3(SYS_open, (int)path, 01 | 0100 | 01000, 0644);
    if (fd < 0) { wr(2, "cannot write shot\n"); return; }
    char hdr[32];
    int hl = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    sys3(SYS_write, fd, (int)hdr, hl);
    static uint8_t row[640 * 3];
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            /* the frame, and the pointer where the screen has it */
            int i_ = y * SCREEN_WIDTH + x;
            uint8_t c = (g_sim_vram[i_] != g_sim_shadow[i_] ? g_sim_vram[i_] : BACKBUF[i_]) & 15;
            row[x * 3] = PAL[c][0]; row[x * 3 + 1] = PAL[c][1]; row[x * 3 + 2] = PAL[c][2];
        }
        sys3(SYS_write, fd, (int)row, SCREEN_WIDTH * 3);
    }
    sys3(SYS_close, fd, 0, 0);
}

/* ── frame loop (mirrors scheduler_kernel_task) ── */
extern bool g_menubar_click_consumed;
void desktop_on_frame(void);
void draw_cursor(int x, int y);
void speaker_update(void);

int sim_verify, sim_checked, sim_mismatch, sim_rangebad, sim_frame_no;
void sim_shot(const char *);
static int s_prev_left;
volatile uint32_t g_frame_mark, g_phase_mark;
void sim_frame(void) {
    (void)g_frame_mark; /* (TLB measurement: a frame starts) */
    pit_ticks += 16;
    pit_seconds = pit_ticks / 1000;
    g_menubar_click_consumed = false;
    { extern void print_pump(void); print_pump(); }
    { extern void audio_update(void); audio_update(); }
    { extern void fdd_poll(void); fdd_poll(); }
    mouse.left_clicked = mouse.left && !s_prev_left;
    s_prev_left = mouse.left;
    { static int pr; mouse.right_clicked = mouse.right && !pr; pr = mouse.right; }
    wm_frame_begin();
    { extern bool wm_switcher_key(void); wm_switcher_key(); }
    wm_sync_menubar(&g_menubar);
    menubar_layout(&g_menubar);
    popup_update();
    menubar_update(&g_menubar);
    ctl_dispatch();
    os_tick_apps();
    wm_update_all();
    os_reap_dead_apps();
    desktop_on_frame();
    { extern void wd_heartbeat(void); wd_heartbeat(); } /* the loop is alive */
    {
        int y0, y1;
        int kind = wm_compose(&g_menubar, &y0, &y1);
        if (sim_verify) { /* lazy result must equal a plain full redraw */
            static unsigned char lazy[640 * 400];
            int n = SCREEN_WIDTH * SCREEN_HEIGHT;
            memcpy(lazy, BACKBUF, n);
            if (wm_debug_reference(&g_menubar)) {
                sim_checked++;
                int bad = -1, cnt = 0;
                for (int i = 0; i < n; i++)
                    if (lazy[i] != BACKBUF[i]) { if (bad < 0) bad = i; cnt++; }
                if (bad >= 0) {
                    sim_mismatch++;
                    if (sim_mismatch <= 5) {
                        char b[120];
                        snprintf(b, sizeof b, "  LAZY MISMATCH frame %d kind %d: %d px, first at %d,%d\n",
                                 sim_frame_no, kind, cnt, bad % SCREEN_WIDTH, bad / SCREEN_WIDTH);
                        wr(1, b);
                        if (sim_mismatch == 1) { sim_shot("mm_ref"); memcpy(BACKBUF, lazy, n); sim_shot("mm_lazy"); }
                    }
                }
                /* the row range must cover every pixel that changed */
                static unsigned char prev[640 * 400]; static int have;
                if (have && kind != WM_FRAME_FULL)
                    for (int i = 0; i < n; i++)
                        if (prev[i] != lazy[i]) {
                            int y = i / SCREEN_WIDTH;
                            if (kind == WM_FRAME_NONE || y < y0 || y > y1) { sim_rangebad++; break; }
                        }
                memcpy(prev, lazy, n); have = 1;
            }
            memcpy(BACKBUF, lazy, n);
        }
        /* to the screen, as the scheduler does */
        { extern void cursor_frame(bool); cursor_frame(true); }
        if (kind == WM_FRAME_FULL) gpu_blit();
        else if (kind == WM_FRAME_PART) gpu_blit_rows(y0, y1);
        else gpu_blit_skip();
        sim_frame_no++;
    }
    kb.key_pressed = false;
    kb.last_char = 0;
    kb.last_scancode = 0;
    mouse.dx = mouse.dy = 0;
}
void sim_frames(int n) { while (n--) sim_frame(); }
void sim_move(int x, int y) { mouse.dx = x - mouse.x; mouse.dy = y - mouse.y; mouse.x = x; mouse.y = y; sim_frame(); }
void sim_click(int x, int y) {
    sim_move(x, y);
    mouse.left = true; sim_frame();
    mouse.left = false; sim_frame();
}
void sim_rclick(int x, int y) {
    sim_move(x, y);
    mouse.right = true; sim_frame();
    mouse.right = false; sim_frame();
}
void sim_dblclick(int x, int y) { sim_click(x, y); sim_click(x, y); }
void sim_drag(int x0, int y0, int x1, int y1) {
    sim_move(x0, y0); mouse.left = true; sim_frame();
    for (int i = 1; i <= 4; i++) sim_move(x0 + (x1 - x0) * i / 4, y0 + (y1 - y0) * i / 4);
    mouse.left = false; sim_frame();
}
/* scancode + char, with modifiers */
void sim_key2(uint8_t sc, char ch, bool shift, bool ctrl) {
    kb.shift = shift; kb.ctrl = ctrl;
    kb_process_byte(sc);           /* sets last_scancode/last_char/ctrl_* */
    if (ch) kb.last_char = ch;
    sim_frame();
    kb.shift = kb.ctrl = false;
}
void sim_key(uint8_t sc) { sim_key2(sc, 0, false, false); }
void sim_ext(uint8_t sc) { kb_process_byte(0xE0); kb_process_byte(sc); sim_frame(); }
void sim_type(const char *s) {
    static const char *rows[] = {"1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "\\zxcvbnm,./"};
    static const uint8_t base[] = {0x02, 0x10, 0x1E, 0x2B};
    static const char *shifted = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
    static const char *plain = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == ' ') { sim_key(0x39); continue; }
        if (c == '\n') { sim_key(0x1C); continue; }
        bool up = false;
        const char *p = strchr(shifted, c);
        if (p) { c = plain[p - shifted]; up = true; }
        for (int r = 0; r < 4; r++) {
            const char *q = strchr(rows[r], c);
            if (q) { sim_key2(base[r] + (q - rows[r]), 0, up, false); break; }
        }
    }
}

app_descriptor *sim_find(const char *name) { return os_find_app(name); }

/* /APPS as the disk image gets it (extapps/MANIFEST, the built .APPs) */
#include "os/appload.h"
static void sim_install_apps(void) {
    static uint8_t buf[64 * 1024];
    static char man[4096];
    uint32_t n = 0;
    fs_rm_rf("/APPS");
    if (!load_file(ROOT "extapps/MANIFEST", (uint8_t *)man, sizeof man - 1, &n)) return;
    man[n] = 0;
    fs_mkdir("/APPS");
    for (char *l = man; *l;) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char *h = strchr(l, '#'); if (h) *h = 0;
        char *bar = strchr(l, '|');
        if (bar) {
            char where[160], what[64], path[200];
            int k = 0; for (char *p = l; p < bar && k < 159; p++) where[k++] = *p;
            while (k && where[k - 1] == ' ') k--; where[k] = 0;
            char *w = bar + 1; while (*w == ' ') w++;
            k = 0; while (w[k] && k < 63) { what[k] = w[k]; k++; }
            while (k && what[k - 1] == ' ') k--; what[k] = 0;
            char *sl = strrchr(where, '/');
            if (sl) { char d[200]; snprintf(d, sizeof d, "/APPS/%.*s", (int)(sl - where), where); fs_mkdir(d); }
            snprintf(path, sizeof path, "/APPS/%s", where);
            fat_file_t f;
            if (what[0] == '@') {
                char t[80]; int m = snprintf(t, sizeof t, "%s\r\n", what + 1);
                if (fs_create(path, &f)) { fs_write(&f, t, m); fs_close(&f); }
            } else {
                char hp[200]; snprintf(hp, sizeof hp, ROOT "build/extapps/%s.APP", what);
                if (load_file(hp, buf, sizeof buf, &n) && fs_create(path, &f)) { fs_write(&f, buf, n); fs_close(&f); }
            }
        }
        if (!e) break;
        l = e + 1;
    }
}

/* a built-in app by its name, else an .APP in /APPS by its header name */
app_instance_t *sim_launch(const char *name) {
    app_descriptor *d = os_find_app(name);
    if (d) return os_launch_app(d);
    char p[512];
    if (!appload_find_named(name, p, sizeof p)) {
        if (!strcmp(name, "AUDIOPLA")) return sim_launch("Audio Player");
        return NULL;
    }
    return appload_launch(p);
}

int scenario(const char *name);
void simvmm_on(void);

const char *sim_arg2;
int sim_main(int argc, char **argv) {
    if (argc > 2) sim_arg2 = argv[2];
    map((uint32_t)BACKBUF, 0x100000);
    uint8_t *heap = (uint8_t *)map(0x10000000, 32 * 1024 * 1024);
    alloc_set_range((uint32_t)heap, (uint32_t)heap + 32 * 1024 * 1024);
    /* frame pool for app images/heaps; "paging" = mprotect (simvmm.c) */
    map(0x30000000, 2 * 1024 * 1024);
    pmm_init(0x30000000, 512);
    simvmm_on();

    g_dk[0] = (uint8_t *)map(0x20000000, 256 * 1024 * 1024);
    g_dk[1] = (uint8_t *)map(0x40000000, 256 * 1024 * 1024);
    uint32_t got = 0;
    if (!load_file("disk.img", g_disk, 256 * 1024 * 1024, &got)) { wr(2, "no disk.img\n"); return 1; }
    g_disk_sectors = got / 512;

    /* a scenario may change the disks first (setup: blank, floppy in A:) */
    extern int sim_pre_mount(const char *sc);
    uint8_t boot_drive = sim_pre_mount(argc > 1 ? argv[1] : "") ? 0x00 : 0x80;
    fs_mount();
    { dir_entry_t de; g_setup_mode = boot_drive < 0x80 && fs_find(SETUP_ZIP, &de); }
    if (!g_setup_mode) sim_install_apps();
    clipboard_init();
    cfg_init_defaults();
    g_cfg.reduce_motion = 1;
    g_cfg.sound_enabled = 0;
    g_cfg.networking_enabled = 0;
    desktop_fs_init();
    wm_init();
    desktop_init();
    menubar_init();
    error_set_gui_mode(true);
    for (int i = 0; i < app_registry_count; i++)
        os_install_app(app_registry[i].desc);
    if (g_setup_mode) { extern app_descriptor setup_app; os_launch_app(&setup_app); }
    mouse.x = 600; mouse.y = 380;
    sim_frames(3);
    const char *sc = argc > 1 ? argv[1] : "desktop";
    if (sc[0] == '+') { sim_verify = 1; sc++; }
    int rc = scenario(sc);
    if (sim_verify) {
        char b[160];
        snprintf(b, sizeof b, "  lazy: full %u part %u none %u | checked %d mismatch %d range-bad %d %s\n",
                 g_wm_frames[2], g_wm_frames[1], g_wm_frames[0], sim_checked, sim_mismatch, sim_rangebad,
                 sim_mismatch || sim_rangebad ? "FAIL" : "ok");
        wr(1, b);
    }
    return rc;
}
void sim_exit0(void) { sys3(SYS_exit, 0, 0, 0); }

/* write a buffer to a host file */
void sim_save(const char *path, const void *d, unsigned n) {
    int fd = sys3(SYS_open, (int)path, 0101 | 01000, 0644); /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd < 0) return;
    sys3(4, fd, (int)d, (int)n);
    sys3(6, fd, 0, 0);
}
void sim_save_hda(const char *path) { sim_disk_save(0, path); }
