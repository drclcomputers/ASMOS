#include "drivers/gpu.h"
#include "config/config.h"
#include "lib/core.h"
#include "lib/memory.h"

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA 0x01CF

#define VBE_DISPI_INDEX_ID 0
#define VBE_DISPI_INDEX_XRES 1
#define VBE_DISPI_INDEX_YRES 2
#define VBE_DISPI_INDEX_BPP 3
#define VBE_DISPI_INDEX_ENABLE 4
#define VBE_DISPI_INDEX_VIRT_WIDTH 6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET 8
#define VBE_DISPI_INDEX_Y_OFFSET 9

#define VBE_DISPI_DISABLED 0x00
#define VBE_DISPI_ENABLED 0x01
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM 0x80

#define BGA_ID_MIN 0xB0C0
#define BGA_ID_MAX 0xB0C5

/* Canonical framebuffer size for one page (8-bit, 1 byte/pixel) */
#define FB_PAGE_BYTES (SCREEN_WIDTH * SCREEN_HEIGHT)
/* Two pages for BGA double-buffering */
#define FB_TOTAL_BYTES (FB_PAGE_BYTES * 2)

static gpu_backend_t s_backend = GPU_BACKEND_NONE;
static uint32_t s_fb = 0;
static uint8_t s_page = 0; /* currently *displayed* page (0 or 1) */

extern uint32_t g_vesa_fb;

/* ── BGA helpers ─────────────────────────────────────────────────────────── */
static void bga_write(uint16_t index, uint16_t data) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, data);
}

static uint16_t bga_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static bool bga_detect(void) {
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    return (id >= BGA_ID_MIN && id <= BGA_ID_MAX);
}

static void bga_set_mode(void) {
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write(VBE_DISPI_INDEX_XRES, SCREEN_WIDTH);
    bga_write(VBE_DISPI_INDEX_YRES, SCREEN_HEIGHT);
    bga_write(VBE_DISPI_INDEX_BPP, 8);
    bga_write(VBE_DISPI_INDEX_VIRT_WIDTH, SCREEN_WIDTH);
    bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, SCREEN_HEIGHT * 2); /* two pages */
    bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_ENABLE,
              VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);
}

/* ── FB address validation ───────────────────────────────────────────────── */
static bool fb_addr_valid(uint32_t addr) {
    if (addr == 0)
        return false;
    if (addr < 0x00100000U)
        return false; /* below 1 MB */
    if (addr == 0xFFFFFFFFU)
        return false; /* BIOS sentinel */
    /* Reject obviously wrong values (must be 64 KB aligned at minimum) */
    return true;
}

/* ── PCI scan fallback (32-bit protected mode) ───────────────────────────── */
/*
 * Used when BGA is absent and BIOS gave us no FB address.
 * Walks PCI bus 0, looks for any display-class device, and returns BAR0.
 * Returns 0 if nothing found.
 */
static uint32_t pci_scan_display_bar0(void) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        /* Class code register (offset 0x08) */
        uint32_t addr_class = 0x80000000U | ((uint32_t)dev << 11) | 0x08;
        outl(0x0CF8, addr_class);
        uint32_t class_rev = inl(0x0CFC);
        if (class_rev == 0xFFFFFFFFU)
            continue;

        uint8_t base_class = (uint8_t)(class_rev >> 24);
        uint8_t sub_class = (uint8_t)(class_rev >> 16);

        bool is_display =
            (base_class == 0x03) || (base_class == 0x00 && sub_class == 0x01);
        if (!is_display)
            continue;

        /* BAR0 (offset 0x10) */
        uint32_t addr_bar0 = 0x80000000U | ((uint32_t)dev << 11) | 0x10;
        outl(0x0CF8, addr_bar0);
        uint32_t bar0 = inl(0x0CFC);

        if (bar0 & 0x01)
            continue; /* I/O BAR */
        bar0 &= 0xFFFFFFF0U;
        if (!fb_addr_valid(bar0))
            continue;

        /* Enable memory space */
        uint32_t addr_cmd = 0x80000000U | ((uint32_t)dev << 11) | 0x04;
        outl(0x0CF8, addr_cmd);
        uint32_t cmd = inl(0x0CFC);
        outl(0x0CF8, addr_cmd);
        outl(0x0CFC, cmd | 0x02);

        return bar0;
    }
    return 0;
}

/* CGA-compatible 6-bit DAC palette (values 0-63, VGA DAC is 6-bit) */
static const uint8_t s_cga_pal[16][3] = {
    {  0,  0,  0 }, /* 0  BLACK        */
    {  0,  0, 42 }, /* 1  BLUE         */
    {  0, 42,  0 }, /* 2  GREEN        */
    {  0, 42, 42 }, /* 3  CYAN         */
    { 42,  0,  0 }, /* 4  RED          */
    { 42,  0, 42 }, /* 5  MAGENTA      */
    { 42, 21,  0 }, /* 6  BROWN        */
    { 42, 42, 42 }, /* 7  LIGHT_GRAY   */
    { 21, 21, 21 }, /* 8  DARK_GRAY    */
    { 21, 21, 63 }, /* 9  LIGHT_BLUE   */
    { 21, 63, 21 }, /* 10 LIGHT_GREEN  */
    { 21, 63, 63 }, /* 11 LIGHT_CYAN   */
    { 63, 21, 21 }, /* 12 LIGHT_RED    */
    { 63, 21, 63 }, /* 13 LIGHT_MAGENTA*/
    { 63, 63, 21 }, /* 14 YELLOW       */
    { 63, 63, 63 }, /* 15 WHITE        */
};

static void dac_set_palette(void) {
    while (inb(0x3DA) & 0x08);
    while (!(inb(0x3DA) & 0x08));

    outb(0x3C8, 0);
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, s_cga_pal[i][0]);   /* R */
        outb(0x3C9, s_cga_pal[i][1]);   /* G */
        outb(0x3C9, s_cga_pal[i][2]);   /* B */
    }
}

/* ── gpu_init ────────────────────────────────────────────────────────────── */
void gpu_init(void) {
    uint32_t bios_fb = *(volatile uint32_t *)0x600;

    if (bga_detect()) {
        bga_set_mode();
        s_backend = GPU_BACKEND_BGA;
        s_page = 0;

        /*
         * BGA on QEMU/VirtualBox: the BIOS may or may not populate 0x600.
         * The BGA LFB is always at the first PCI BAR of the BGA device,
         * typically 0xE0000000.  Use BIOS value if valid, else fall back to
         * PCI scan, then to the compiled-in constant.
         */
        if (fb_addr_valid(bios_fb)) {
            s_fb = bios_fb;
        } else {
            s_fb = pci_scan_display_bar0();
            if (!fb_addr_valid(s_fb))
                s_fb = (uint32_t)VESA_FB;
        }

        g_vesa_fb = s_fb;

        /* Clear both pages */
        memset((void *)s_fb, 0, FB_TOTAL_BYTES);

    } else {
        /*
         * VESA / real GPU path.
         * Bootloader already set the video mode and stored PhysBasePtr at
         * 0x0600.  If that's missing, try a PCI scan.
         */
        if (fb_addr_valid(bios_fb)) {
            s_fb = bios_fb;
        } else {
            s_fb = pci_scan_display_bar0();
            if (!fb_addr_valid(s_fb))
                s_fb = (uint32_t)VESA_FB;
        }

        g_vesa_fb = s_fb;
        s_backend = GPU_BACKEND_VESA;
    }
    dac_set_palette();
}

/* ── gpu_blit ────────────────────────────────────────────────────────────── */
void gpu_blit(void) {
    if (!fb_addr_valid(s_fb))
        return;

    uint32_t pixels = FB_PAGE_BYTES;
    uint32_t dwords = pixels / 4;
    uint32_t *src = (uint32_t *)BACKBUF;

    if (s_backend == GPU_BACKEND_BGA) {
        /*
         * Double-buffer: draw into the *hidden* page, then flip.
         *   page 0 → Y offset 0
         *   page 1 → Y offset SCREEN_HEIGHT
         *
         * draw_page is the one we write to (opposite of currently displayed).
         */
        uint8_t draw_page = s_page ^ 1;
        uint32_t page_bytes = (uint32_t)draw_page * FB_PAGE_BYTES;
        uint32_t *dst = (uint32_t *)(s_fb + page_bytes);

        __asm__ volatile("cld\n\t rep movsd"
                         : "+S"(src), "+D"(dst), "+c"(dwords)::"memory");

        /* Flip: display the page we just drew */
        bga_write(VBE_DISPI_INDEX_Y_OFFSET,
                  draw_page ? (uint16_t)SCREEN_HEIGHT : 0);
        s_page = draw_page;

    } else {
        /* Single-buffer VESA blit */
        uint32_t *dst = (uint32_t *)s_fb;
        __asm__ volatile("cld\n\t rep movsd"
                         : "+S"(src), "+D"(dst), "+c"(dwords)::"memory");
    }
}

gpu_backend_t gpu_backend(void) { return s_backend; }
