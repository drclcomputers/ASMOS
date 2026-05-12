#include "drivers/gpu.h"
#include "config/config.h"
#include "lib/core.h"
#include "lib/memory.h"

/*
 * gpu.c – real-hardware hardened
 *
 * S3 Trio64 (Diamond Stealth64 DRAM) specifics:
 *   • PCI VID:DID = 0x5333:0x8811 (S3 Trio64)
 *                   0x5333:0x8814 (S3 Trio64 V+)
 *   • BAR0 is the linear framebuffer (memory-mapped, 64-bit window in 32-bit
 *     address space, typically at 0xE0000000 but BIOS-assigned).
 *   • The card has no BGA.  bga_detect() will always return false on it.
 *   • VBE BIOS: version 1.2 on most Diamond BIOSes; may or may not expose
 *     PhysBasePtr in ModeInfoBlock (bit 7 of ModeAttributes).
 *     If BIOS provided PhysBasePtr (stored at 0x0600 by stage2) use it.
 *     Otherwise fall back to PCI BAR0 scan.
 *
 * PCI presence test:
 *   Before walking the PCI config space we verify that PCI Mechanism 1 is
 *   present by writing 0x80000000 to CF8h and reading it back.  On pure
 *   ISA/VL-Bus machines this read returns 0xFFFFFFFF or garbage, so we
 *   skip the scan and avoid bus faults.
 *
 * Memory layout (4 MB machine):
 *   0x00000000 – 0x000FFFFF  Real-mode/BIOS area (1 MB)
 *   0x00100000 – 0x0014AFFF  Back-buffer  (307 200 bytes, 640×480×1)
 *   0x0014B000 – 0x003EFFFF  Heap         (~2.6 MB)
 *   0x003F0000 – 0x003F7FFF  Kernel stack (32 KB)
 *   0x003F8000 – 0x003FFFFF  Reserved
 *   Framebuffer lives at the card's PCI BAR (0xE0000000 typical) – outside
 *   the 4 MB physical RAM window, so no aliasing with system RAM.
 */

/* ── BGA (Bochs/QEMU only) registers ────────────────────────────────────────*/
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

/* One page = 640×480 = 307 200 bytes */
#define FB_PAGE_BYTES (SCREEN_WIDTH * SCREEN_HEIGHT)
/* Two pages for BGA double-buffering */
#define FB_TOTAL_BYTES (FB_PAGE_BYTES * 2)

/* ── S3 Trio64 PCI identification
 * ────────────────────────────────────────────*/
#define S3_VENDOR_ID 0x5333u
#define S3_TRIO64_DID 0x8811u   /* Trio64 */
#define S3_TRIO64VP_DID 0x8814u /* Trio64 V+ */

static gpu_backend_t s_backend = GPU_BACKEND_NONE;
static uint32_t s_fb = 0;
static uint8_t s_page = 0; /* currently displayed page (BGA only) */

extern uint32_t g_vesa_fb;

/* ── BGA helpers
 * ─────────────────────────────────────────────────────────────*/
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
    bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, SCREEN_HEIGHT * 2);
    bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_ENABLE,
              VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);
}

/* ── FB address validation
 * ───────────────────────────────────────────────────*/
static bool fb_addr_valid(uint32_t addr) {
    if (addr == 0)
        return false;
    if (addr < 0x00100000U)
        return false; /* below 1 MB – can't be VESA FB */
    if (addr == 0xFFFFFFFFU)
        return false; /* unconfigured BAR sentinel */
    /* Must be at least 64 KB aligned (all real VGA BARs are page-aligned) */
    if (addr & 0x0000FFFFU)
        return false;
    return true;
}

/* ── PCI Mechanism 1 presence test ──────────────────────────────────────────*/
/*
 * Write the enable bit + bus 0, device 0, function 0 to CF8h and read back.
 * On a machine with no PCI this returns 0xFFFFFFFF or the written value is
 * not preserved.  On a proper PCI host we read back exactly what we wrote.
 */
static bool pci_mechanism1_present(void) {
    outl(0x0CF8, 0x80000000U);
    return (inl(0x0CF8) == 0x80000000U);
}

/* ── PCI config-space read (Mechanism 1)
 * ─────────────────────────────────────*/
static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (reg & 0xFC);
    outl(0x0CF8, addr);
    return inl(0x0CFC);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                        uint32_t val) {
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (reg & 0xFC);
    outl(0x0CF8, addr);
    outl(0x0CFC, val);
}

/* ── PCI display device scan
 * ─────────────────────────────────────────────────*/
/*
 * Walk bus 0 looking for:
 *   1. S3 Trio64 specifically (VID=5333h, DID=8811h or 8814h) – checked first.
 *   2. Any PCI display-class device (class 03h, or class 00h sub 01h) as
 *      a generic fallback.
 *
 * Returns BAR0 (memory-space, masked) or 0 if nothing found.
 *
 * Also enables PCI Memory Space access (command register bit 1) for the
 * found device so the framebuffer becomes accessible without a full BIOS
 * POST.  This is important when booting from a cold state where the BIOS
 * may not have issued a full VGA BIOS initialisation on secondary adapters.
 */
static uint32_t pci_scan_display_bar0(void) {
    if (!pci_mechanism1_present())
        return 0;

    uint32_t best_bar = 0;
    uint8_t best_dev = 0xFF;
    uint8_t best_fn = 0;
    bool found_s3 = false;

    for (uint8_t dev = 0; dev < 32; dev++) {
        /* Check functions 0 and 1 (multi-function cards are rare but exist) */
        for (uint8_t fn = 0; fn < 2; fn++) {
            uint32_t vid_did = pci_read32(0, dev, fn, 0x00);
            if (vid_did == 0xFFFFFFFFU)
                continue; /* slot empty */

            uint16_t vendor = (uint16_t)(vid_did & 0xFFFFU);
            uint16_t device = (uint16_t)(vid_did >> 16);

            /* Class code register */
            uint32_t class_rev = pci_read32(0, dev, fn, 0x08);
            uint8_t base_class = (uint8_t)(class_rev >> 24);
            uint8_t sub_class = (uint8_t)(class_rev >> 16);

            bool is_s3_trio =
                (vendor == S3_VENDOR_ID) &&
                (device == S3_TRIO64_DID || device == S3_TRIO64VP_DID);

            bool is_display = (base_class == 0x03) ||
                              (base_class == 0x00 && sub_class == 0x01);

            if (!is_s3_trio && !is_display)
                continue;

            /* Read BAR0 */
            uint32_t bar0 = pci_read32(0, dev, fn, 0x10);
            if (bar0 & 0x01U)
                continue; /* I/O-space BAR, not memory */
            bar0 &= 0xFFFFFFF0U;

            if (!fb_addr_valid(bar0))
                continue;

            /* Prefer S3 over generic display device */
            if (is_s3_trio && !found_s3) {
                found_s3 = true;
                best_bar = bar0;
                best_dev = dev;
                best_fn = fn;
            } else if (!found_s3 && best_dev == 0xFF) {
                best_bar = bar0;
                best_dev = dev;
                best_fn = fn;
            }
        }
    }

    if (best_dev == 0xFF)
        return 0;

    /* Enable Memory Space (bit 1) in command register */
    uint32_t cmd = pci_read32(0, best_dev, best_fn, 0x04);
    pci_write32(0, best_dev, best_fn, 0x04, cmd | 0x02U);

    return best_bar;
}

/* ── VGA DAC palette
 * ─────────────────────────────────────────────────────────*/
/*
 * CGA-compatible 16-colour palette in 6-bit DAC format.
 * Indices 0-15 → CGA colours.
 * Indices 16-255 → zeroed (prevents BIOS splash garbage in 8bpp mode).
 *
 * NOTE: We intentionally do NOT spinloop on VSYNC (port 0x3DA bit 3).
 * On S3 Trio64 in VESA LFB mode the input-status register is not reliably
 * wired to the display engine's blanking signal, so the spinloop either
 * blocks a full frame or never exits.  The DAC write sequence (index via
 * 0x3C8, three data bytes via 0x3C9) is atomic with respect to palette
 * display and can be issued at any time safely.
 */
static const uint8_t s_cga_pal[16][3] = {
    {0, 0, 0},    /*  0 BLACK         */
    {0, 0, 42},   /*  1 BLUE          */
    {0, 42, 0},   /*  2 GREEN         */
    {0, 42, 42},  /*  3 CYAN          */
    {42, 0, 0},   /*  4 RED           */
    {42, 0, 42},  /*  5 MAGENTA       */
    {42, 21, 0},  /*  6 BROWN         */
    {42, 42, 42}, /*  7 LIGHT_GRAY    */
    {21, 21, 21}, /*  8 DARK_GRAY     */
    {21, 21, 63}, /*  9 LIGHT_BLUE    */
    {21, 63, 21}, /* 10 LIGHT_GREEN   */
    {21, 63, 63}, /* 11 LIGHT_CYAN    */
    {63, 21, 21}, /* 12 LIGHT_RED     */
    {63, 21, 63}, /* 13 LIGHT_MAGENTA */
    {63, 63, 21}, /* 14 YELLOW        */
    {63, 63, 63}, /* 15 WHITE         */
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

/* ── gpu_init
 * ────────────────────────────────────────────────────────────────*/
void gpu_init(void) {
    /*
     * Stage2 stores the VESA PhysBasePtr at physical address 0x0600 (dword).
     * A value of 0 means either:
     *   (a) stage2 fell through to banked/VGA mode (no LFB reported), or
     *   (b) VESA was not available at all (VGA fallback).
     * In both cases we attempt PCI discovery.
     */
    uint32_t bios_fb = *(volatile uint32_t *)0x0600;

    if (bga_detect()) {
        /* ── QEMU/VirtualBox BGA path ────────────────────────────────────── */
        bga_set_mode();
        s_backend = GPU_BACKEND_BGA;
        s_page = 0;

        if (fb_addr_valid(bios_fb)) {
            s_fb = bios_fb;
        } else {
            s_fb = pci_scan_display_bar0();
            if (!fb_addr_valid(s_fb))
                s_fb = (uint32_t)VESA_FB;
        }

        g_vesa_fb = s_fb;

        if (fb_addr_valid(s_fb))
            memset((void *)s_fb, 0, FB_TOTAL_BYTES);

    } else {
        /* ── Real hardware path (S3 Trio64 / VESA LFB / VGA) ────────────── */
        s_backend = GPU_BACKEND_VESA;

        if (fb_addr_valid(bios_fb)) {
            /*
             * Stage2 successfully got PhysBasePtr from VBE ModeInfoBlock.
             * This is the normal S3 Trio64 LFB path.
             */
            s_fb = bios_fb;
        } else {
            /*
             * No BIOS-provided address.  Scan PCI for the S3 card's BAR0.
             * This covers:
             *   • Banked VESA mode (stage2 set mode but stored FB=0)
             *   • VGA fallback mode (0xA0000 for mode 13h, but we'll use
             *     the LFB via BAR0 at the proper address instead)
             */
            s_fb = pci_scan_display_bar0();

            if (!fb_addr_valid(s_fb)) {
                /*
                 * PCI scan found nothing (no PCI, or BAR unset).
                 * Last resort: use the compile-time constant.
                 * On S3 Trio64 the BIOS usually maps the LFB at 0xE0000000.
                 */
                s_fb = (uint32_t)VESA_FB;
            }
        }

        g_vesa_fb = s_fb;
    }

    dac_set_palette();
}

/* ── gpu_blit
 * ────────────────────────────────────────────────────────────────*/
void gpu_blit(void) {
    if (!fb_addr_valid(s_fb))
        return;

    uint32_t pixels = FB_PAGE_BYTES;
    uint32_t dwords = pixels / 4;
    uint32_t *src = (uint32_t *)BACKBUF;

    if (s_backend == GPU_BACKEND_BGA) {
        /*
         * BGA double-buffer: draw into the hidden page, then page-flip via
         * the Y-offset register.
         */
        uint8_t draw_page = s_page ^ 1;
        uint32_t page_bytes = (uint32_t)draw_page * FB_PAGE_BYTES;
        uint32_t *dst = (uint32_t *)(s_fb + page_bytes);

        __asm__ volatile("cld\n\t rep movsd"
                         : "+S"(src), "+D"(dst), "+c"(dwords)::"memory");

        bga_write(VBE_DISPI_INDEX_Y_OFFSET,
                  draw_page ? (uint16_t)SCREEN_HEIGHT : 0);
        s_page = draw_page;

    } else {
        /*
         * Single-buffer blit for VESA LFB (S3 Trio64 normal path).
         * If the card is in banked mode (s_fb==0) fb_addr_valid guards above.
         */
        uint32_t *dst = (uint32_t *)s_fb;
        __asm__ volatile("cld\n\t rep movsd"
                         : "+S"(src), "+D"(dst), "+c"(dwords)::"memory");
    }
}

gpu_backend_t gpu_backend(void) { return s_backend; }
