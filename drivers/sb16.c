#include "drivers/sb16.h"
#include "lib/core.h"
#include "lib/memory.h"

// ── Port offsets
// ──────────────────────────────────────────────────────────────
#define DSP_RESET (s_port + 0x6)
#define DSP_READ (s_port + 0xA)
#define DSP_WRITE (s_port + 0xC)
#define DSP_READ_STATUS (s_port + 0xE)
#define DSP_ACK_8 (s_port + 0xE)
#define DSP_ACK_16 (s_port + 0xF)
#define MIXER_ADDR (s_port + 0x4)
#define MIXER_DATA (s_port + 0x5)

// ── DSP commands
// ──────────────────────────────────────────────────────────────
#define DSP_CMD_SET_RATE 0x41
#define DSP_CMD_PLAY_8BIT 0xC0 // single-cycle
#define DSP_CMD_HALT_8 0xD0
#define DSP_CMD_SPEAKER_ON 0xD1
#define DSP_CMD_SPEAKER_OFF 0xD3
#define DSP_CMD_GET_VERSION 0xE1

// ── 8-bit DMA channel 1
// ───────────────────────────────────────────────────────
#define DMA8_MASK 0x0A
#define DMA8_MODE 0x0B
#define DMA8_CLEAR_FF 0x0C
#define DMA8_ADDR_CH1 0x02
#define DMA8_COUNT_CH1 0x03
#define DMA8_PAGE_CH1 0x83

// ── Mixer registers
// ───────────────────────────────────────────────────────────
#define MIXER_IRQ_REG 0x80
#define MIXER_MASTER_VOL 0x22
#define MIXER_DAC_VOL 0x04
#define MIXER_LINE_VOL 0x2E
#define MIXER_FM_VOL 0x26
#define MIXER_FM_VOL_LEFT 0x34
#define MIXER_FM_VOL_RIGHT 0x35

// ── PIC / IRQ5
// ────────────────────────────────────────────────────────────────
#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC1_EOI 0x20

// ── DMA buffer
// ────────────────────────────────────────────────────────────────
// 0x70000–0x74000: 7th 64K page, does not cross a 64K boundary.
#define DMA_BUF_PHYS 0x70000
#define DMA_BUF_SIZE 0x4000

// ── Spin-loop timeouts
// ──────────────────────────────────────────────────────── dsp_write: SB16 spec
// says write buffer clears within a few µs. 0x10000 iterations at ~10ns each ≈
// 655µs — plenty, never hangs the CPU.
#define DSP_WRITE_TIMEOUT 0x10000u
// dsp_read: DSP should reply within 1ms after a command.
// 0x40000 iterations ≈ 2.6ms.
#define DSP_READ_TIMEOUT 0x40000u

// ── Module state
// ──────────────────────────────────────────────────────────────
static uint16_t s_port = SB16_DEFAULT_PORT;
static bool s_detected = false;
static volatile bool s_playing = false;

static const uint8_t *s_src_data = NULL;
static uint32_t s_src_len = 0;
static uint32_t s_src_offset = 0;
static uint32_t s_sample_rate = 22050;
static bool s_looping = false;

// ── Low-level DSP helpers — ALL have bounded timeouts
// ─────────────────────────

static bool dsp_reset(void) {
    outb(DSP_RESET, 1);
    for (volatile int i = 0; i < 300; i++)
        __asm__ volatile("nop");
    outb(DSP_RESET, 0);
    for (volatile uint32_t i = 0; i < 0x10000u; i++) {
        if ((inb(DSP_READ_STATUS) & 0x80) && inb(DSP_READ) == 0xAA)
            return true;
    }
    return false; // card not present / not responding
}

// Returns false if the DSP write buffer never cleared (card stuck or absent).
static bool dsp_write(uint8_t val) {
    for (uint32_t i = 0; i < DSP_WRITE_TIMEOUT; i++) {
        if (!(inb(DSP_WRITE) & 0x80)) {
            outb(DSP_WRITE, val);
            return true;
        }
    }
    return false; // timeout — treat as card error
}

// Returns -1 on timeout.
static int dsp_read_byte(void) {
    for (uint32_t i = 0; i < DSP_READ_TIMEOUT; i++) {
        if (inb(DSP_READ_STATUS) & 0x80)
            return (int)(uint8_t)inb(DSP_READ);
    }
    return -1; // timeout
}

void mixer_write(uint8_t reg, uint8_t val) {
    outb(MIXER_ADDR, reg);
    outb(MIXER_DATA, val);
}

static uint8_t mixer_read(uint8_t reg) {
    outb(MIXER_ADDR, reg);
    return inb(MIXER_DATA);
}

// ── DMA setup
// ─────────────────────────────────────────────────────────────────
static void dma8_setup(uint32_t phys_addr, uint32_t byte_count) {
    outb(DMA8_MASK, 0x05); // mask channel 1
    outb(DMA8_CLEAR_FF, 0x00);
    outb(DMA8_MODE, 0x49); // single, read (mem→device), ch1
    outb(DMA8_ADDR_CH1, (uint8_t)(phys_addr & 0xFF));
    outb(DMA8_ADDR_CH1, (uint8_t)((phys_addr >> 8) & 0xFF));
    outb(DMA8_PAGE_CH1, (uint8_t)((phys_addr >> 16) & 0xFF));
    uint16_t count = (uint16_t)(byte_count - 1);
    outb(DMA8_COUNT_CH1, (uint8_t)(count & 0xFF));
    outb(DMA8_COUNT_CH1, (uint8_t)((count >> 8) & 0xFF));
    outb(DMA8_MASK, 0x01); // unmask channel 1
}

// ── PIC IRQ5
// ──────────────────────────────────────────────────────────────────
static void pic_unmask_irq5(void) {
    outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 5));
}
static void pic_mask_irq5(void) { outb(PIC1_DATA, inb(PIC1_DATA) | (1 << 5)); }

// ── Helper: program DSP for next chunk ───────────────────────────────────────
// Returns false if any DSP write timed out (card died mid-stream).
static bool dsp_program_chunk(uint32_t count_minus_1) {
    if (!dsp_write(DSP_CMD_SET_RATE))
        return false;
    if (!dsp_write((uint8_t)((s_sample_rate >> 8) & 0xFF)))
        return false;
    if (!dsp_write((uint8_t)(s_sample_rate & 0xFF)))
        return false;
    if (!dsp_write(DSP_CMD_PLAY_8BIT))
        return false;
    if (!dsp_write(0x00))
        return false; // unsigned mono
    if (!dsp_write((uint8_t)(count_minus_1 & 0xFF)))
        return false;
    if (!dsp_write((uint8_t)((count_minus_1 >> 8) & 0xFF)))
        return false;
    return true;
}

// ── IRQ5 handler — called from isr_sb16 trampoline ───────────────────────────
void sb16_irq_handler(void) {
    if (!s_detected) {
        outb(PIC1_CMD, PIC1_EOI);
        return;
    }

    (void)inb(DSP_ACK_8);

    if (!s_playing) {
        outb(PIC1_CMD, PIC1_EOI);
        return;
    }

    if (s_src_data && s_src_offset < s_src_len) {
        uint32_t remaining = s_src_len - s_src_offset;
        uint32_t chunk = (remaining < DMA_BUF_SIZE) ? remaining : DMA_BUF_SIZE;

        uint8_t *dma_buf = (uint8_t *)DMA_BUF_PHYS;
        memcpy(dma_buf, s_src_data + s_src_offset, chunk);
        if (chunk < DMA_BUF_SIZE)
            memset(dma_buf + chunk, 0x80, DMA_BUF_SIZE - chunk);
        s_src_offset += chunk;

        dma8_setup(DMA_BUF_PHYS, DMA_BUF_SIZE);
        if (!dsp_program_chunk(DMA_BUF_SIZE - 1)) {
            // DSP hung mid-stream; abort cleanly
            s_playing = false;
            s_src_data = NULL;
        }

    } else if (s_looping && s_src_data) {
        s_src_offset = 0;
        uint8_t *dma_buf = (uint8_t *)DMA_BUF_PHYS;
        memset(dma_buf, 0x80, DMA_BUF_SIZE);
        dma8_setup(DMA_BUF_PHYS, DMA_BUF_SIZE);
        dsp_program_chunk(DMA_BUF_SIZE - 1);

    } else {
        s_playing = false;
        s_src_data = NULL;
        // dsp_write may time out here if card died, but we're stopping anyway
        dsp_write(DSP_CMD_HALT_8);
    }
    outb(PIC1_CMD, PIC1_EOI);
}

// ── Public API
// ────────────────────────────────────────────────────────────────

bool sb16_init(void) {
    uint16_t ports[] = {0x220, 0x240, 0x260, 0x280};
    for (int i = 0; i < 4; i++) {
        s_port = ports[i];
        if (!dsp_reset())
            continue;

        // Version command — uses bounded helpers now
        if (!dsp_write(DSP_CMD_GET_VERSION))
            continue;
        int major = dsp_read_byte();
        int minor = dsp_read_byte();
        if (major < 0 || minor < 0)
            continue; // timeout → try next port
        if ((uint8_t)major < 4)
            continue; // not SB16

        s_detected = true;
        dsp_write(DSP_CMD_SPEAKER_ON);

        mixer_write(MIXER_MASTER_VOL, 0xFF);
        mixer_write(MIXER_FM_VOL, 0xFF);
        mixer_write(MIXER_DAC_VOL, 0xFF);
        mixer_write(MIXER_LINE_VOL, 0xFF);

        (void)mixer_read(MIXER_IRQ_REG);
        return true;
    }
    // No card found — s_detected stays false, all play calls become no-ops
    return false;
}

bool sb16_detected(void) { return s_detected; }

void sb16_set_volume(uint8_t left, uint8_t right) {
    if (!s_detected)
        return;
    uint8_t val = (uint8_t)((left & 0xF0) | (right >> 4));
    mixer_write(MIXER_DAC_VOL, val);
    mixer_write(MIXER_MASTER_VOL, val);
}

void sb16_play_pcm(const uint8_t *data, uint32_t len, uint32_t sample_rate,
                   bool stereo) {
    if (!s_detected || !data || len == 0)
        return;
    (void)stereo;

    sb16_stop();

    s_src_data = data;
    s_src_len = len;
    s_src_offset = 0;
    s_sample_rate = sample_rate;
    s_looping = false;
    s_playing = true;

    uint32_t chunk = (len < DMA_BUF_SIZE) ? len : DMA_BUF_SIZE;
    uint8_t *dma_buf = (uint8_t *)DMA_BUF_PHYS;
    memcpy(dma_buf, data, chunk);
    if (chunk < DMA_BUF_SIZE)
        memset(dma_buf + chunk, 0x80, DMA_BUF_SIZE - chunk);
    s_src_offset = chunk;

    dma8_setup(DMA_BUF_PHYS, DMA_BUF_SIZE);
    pic_unmask_irq5();

    if (!dsp_program_chunk(DMA_BUF_SIZE - 1)) {
        // DSP didn't accept the command — abort
        s_playing = false;
        s_src_data = NULL;
        pic_mask_irq5();
    }
}

void sb16_stop(void) {
    if (!s_detected)
        return;
    dsp_write(DSP_CMD_HALT_8); // best-effort; may time out if card is gone
    pic_mask_irq5();
    s_playing = false;
    s_src_data = NULL;
}

bool sb16_is_playing(void) { return s_playing; }

void sb16_unmute_fm(void) {
    if (!sb16_detected())
        return;

    mixer_write(MIXER_FM_VOL_LEFT, 0xFF);
    mixer_write(MIXER_FM_VOL_RIGHT, 0xFF);
}
