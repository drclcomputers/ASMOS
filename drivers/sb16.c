#include "drivers/sb16.h"
#include "lib/core.h"
#include "lib/memory.h"

#define DSP_RESET (s_port + 0x6)
#define DSP_READ (s_port + 0xA)
#define DSP_WRITE (s_port + 0xC)
#define DSP_READ_STATUS (s_port + 0xE)
#define DSP_ACK_16 (s_port + 0xF)
#define MIXER_ADDR (s_port + 0x4)
#define MIXER_DATA (s_port + 0x5)

#define DSP_CMD_SET_RATE 0x41
#define DSP_CMD_PLAY_16BIT 0xB6
#define DSP_CMD_PLAY_8BIT 0xC6
#define DSP_CMD_HALT_8 0xD0
#define DSP_CMD_HALT_16 0xD5
#define DSP_CMD_SPEAKER_ON 0xD1
#define DSP_CMD_SPEAKER_OFF 0xD3
#define DSP_CMD_GET_VERSION 0xE1

#define DMA_PAGE_CH1 0x83
#define DMA_ADDR_CH1 0x02
#define DMA_COUNT_CH1 0x03
#define DMA_MASK 0x0A
#define DMA_MODE 0x0B
#define DMA_CLEAR_FF 0x0C
#define DMA_SINGLE_MASK 0x0A

#define DMA_BUF_SIZE 0x4000

static uint16_t s_port = SB16_DEFAULT_PORT;
static bool s_detected = false;
static bool s_playing = false;
#define DMA_BUF_ADDR 0x70000

static bool dsp_reset(void) {
    outb(DSP_RESET, 1);
    uint32_t i = 0;
    while (i++ < 100)
        __asm__ volatile("nop");
    outb(DSP_RESET, 0);
    i = 0;
    while (i++ < 10000) {
        if ((inb(DSP_READ_STATUS) & 0x80) && inb(DSP_READ) == 0xAA)
            return true;
    }
    return false;
}

static void dsp_write(uint8_t val) {
    while (inb(DSP_WRITE) & 0x80) {
    }
    outb(DSP_WRITE, val);
}

static uint8_t dsp_read(void) {
    while (!(inb(DSP_READ_STATUS) & 0x80)) {
    }
    return inb(DSP_READ);
}

static void mixer_write(uint8_t reg, uint8_t val) {
    outb(MIXER_ADDR, reg);
    outb(MIXER_DATA, val);
}

bool sb16_init(void) {
    uint16_t ports[] = {0x220, 0x240, 0x260, 0x280};
    for (int i = 0; i < 4; i++) {
        s_port = ports[i];
        if (dsp_reset()) {
            dsp_write(DSP_CMD_GET_VERSION);
            uint8_t major = dsp_read();
            uint8_t minor = dsp_read();
            if (major >= 4) {
                s_detected = true;
                dsp_write(DSP_CMD_SPEAKER_ON);
                mixer_write(0x22, 0xCC);
                mixer_write(0x26, 0xCC);
                mixer_write(0x04, 0xCC);
                return true;
            }
            (void)minor;
        }
    }
    return false;
}

bool sb16_detected(void) { return s_detected; }

void sb16_set_volume(uint8_t left, uint8_t right) {
    if (!s_detected)
        return;
    uint8_t val = (left & 0xF0) | (right >> 4);
    mixer_write(0x04, val);
    mixer_write(0x22, val);
}

static void dma_setup(uint32_t addr, uint32_t len) {
    outb(DMA_MASK, 0x05);
    outb(DMA_CLEAR_FF, 0x00);
    outb(DMA_MODE, 0x59);
    outb(DMA_ADDR_CH1, addr & 0xFF);
    outb(DMA_ADDR_CH1, (addr >> 8) & 0xFF);
    outb(DMA_PAGE_CH1, (addr >> 16) & 0xFF);
    uint32_t count = len - 1;
    outb(DMA_COUNT_CH1, count & 0xFF);
    outb(DMA_COUNT_CH1, (count >> 8) & 0xFF);
    outb(DMA_MASK, 0x01);
}

void sb16_play_pcm(const uint8_t *data, uint32_t len, uint32_t sample_rate,
                   bool stereo) {
    if (!s_detected)
        return;
    sb16_stop();

    uint32_t copy = len > DMA_BUF_SIZE ? DMA_BUF_SIZE : len;
    uint8_t *dma_buf = (uint8_t *)DMA_BUF_ADDR;
    memcpy(dma_buf, data, copy);
    uint32_t addr = DMA_BUF_ADDR;
    dma_setup(addr, copy);

    dsp_write(DSP_CMD_SET_RATE);
    dsp_write((sample_rate >> 8) & 0xFF);
    dsp_write(sample_rate & 0xFF);

    uint32_t count = copy - 1;
    uint8_t mode = stereo ? 0x30 : 0x10;
    dsp_write(DSP_CMD_PLAY_8BIT);
    dsp_write(mode);
    dsp_write(count & 0xFF);
    dsp_write((count >> 8) & 0xFF);

    s_playing = true;
}

void sb16_stop(void) {
    if (!s_detected)
        return;
    dsp_write(DSP_CMD_HALT_8);
    s_playing = false;
}

bool sb16_is_playing(void) { return s_playing; }

void sb16_update(void) {
    if (!s_playing)
        return;
    if (inb(DSP_READ_STATUS) & 0x80) {
        inb(DSP_READ);
        s_playing = false;
    }
}
