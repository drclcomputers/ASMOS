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
#define VBE_DISPI_INDEX_BANK 5
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

static gpu_backend_t s_backend = GPU_BACKEND_NONE;
static uint32_t s_fb = 0;
static uint8_t s_page = 0;

extern uint32_t g_vesa_fb;

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

void gpu_init(void) {
    s_fb = *(volatile uint32_t *)0x600;
    g_vesa_fb = s_fb;

    if (bga_detect()) {
        bga_set_mode();
        s_backend = GPU_BACKEND_BGA;
        s_page = 0;

        memset((void *)s_fb, 0, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    } else {
        s_backend = GPU_BACKEND_VESA;
    }
}

void gpu_blit(void) {
    if (s_backend == GPU_BACKEND_BGA) {
        uint32_t pixels = SCREEN_WIDTH * SCREEN_HEIGHT;

        uint8_t draw_page = s_page ^ 1;
        uint32_t dst_offset = draw_page ? (uint32_t)SCREEN_HEIGHT : 0;
        uint32_t *src = (uint32_t *)BACKBUF;
        uint32_t *dst = (uint32_t *)(s_fb + dst_offset * SCREEN_WIDTH);
        uint32_t dwords = pixels / 4;

        __asm__ volatile("cld\n\t rep movsd"
                         : "+S"(src), "+D"(dst), "+c"(dwords)::"memory");

        bga_write(VBE_DISPI_INDEX_Y_OFFSET, draw_page ? SCREEN_HEIGHT : 0);

        s_page = draw_page;

    } else {
        uint32_t pixels = SCREEN_WIDTH * SCREEN_HEIGHT;
        uint32_t *src = (uint32_t *)BACKBUF;
        uint32_t *dst = (uint32_t *)s_fb;
        uint32_t dwords = pixels / 4;

        __asm__ volatile("cld\n\t rep movsd"
                         : "+S"(src), "+D"(dst), "+c"(dwords)::"memory");
    }
}

gpu_backend_t gpu_backend(void) { return s_backend; }
