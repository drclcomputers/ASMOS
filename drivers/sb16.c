// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/sb16.h"
#include "interrupts/idt.h"
#include "lib/core.h"
#include "lib/io.h"
#include "lib/memory.h"
#include "lib/time.h"

#define DSP_RESET (s_port + 0x6)
#define DSP_READ (s_port + 0xA)
#define DSP_WRITE (s_port + 0xC)
#define DSP_READ_STATUS (s_port + 0xE)
#define DSP_ACK_8 (s_port + 0xE)
#define MIXER_ADDR (s_port + 0x4)
#define MIXER_DATA (s_port + 0x5)

#define DSP_CMD_TIME_CONST 0x40  /* all versions */
#define DSP_CMD_SET_RATE 0x41    /* SB16 */
#define DSP_CMD_DMA8_SINGLE 0x14 /* all versions */
#define DSP_CMD_PLAY_8BIT 0xC0   /* SB16 */
#define DSP_CMD_HALT_8 0xD0
#define DSP_CMD_SPEAKER_ON 0xD1
#define DSP_CMD_GET_VERSION 0xE1
#define DSP_CMD_RAISE_IRQ 0xF2

#define DMA8_MASK 0x0A
#define DMA8_MODE 0x0B
#define DMA8_CLEAR_FF 0x0C
static const uint8_t DMA8_PAGE[4] = {0x87, 0x83, 0x81, 0x82};

#define MIXER_MASTER_VOL 0x22
#define MIXER_DAC_VOL 0x04
#define MIXER_LINE_VOL 0x2E
#define MIXER_FM_VOL 0x26
#define MIXER_CD_VOL 0x28 /* the CD drive's audio cable (0 after a reset) */
#define MIXER_FM_VOL_LEFT 0x34 /* SB16 only */
#define MIXER_FM_VOL_RIGHT 0x35

/* 16 KB transfer buffer, allocated at init and aligned to 16 KB so it never
 * crosses a 64 KB DMA page; must be below 16 MB (ISA DMA). */
#define DMA_BUF_SIZE 0x4000
static uint8_t *s_dma_buf;
#define DMA_BUF_PHYS ((uint32_t)s_dma_buf)

#define DSP_WRITE_TIMEOUT 0x10000u
#define DSP_READ_TIMEOUT 0x40000u

static uint16_t s_port = SB16_DEFAULT_PORT;
static int s_irq = 5, s_dma = 1;
static uint8_t s_major, s_minor;
static bool s_detected = false;
static volatile bool s_playing = false;

static const uint8_t *s_src_data = NULL;
static uint32_t s_src_len = 0, s_src_offset = 0;
static uint32_t s_rate = 22050; /* what the card plays */
static uint32_t s_step = 1;     /* source samples per played sample */
static bool s_looping = false;
/* paused: the block in flight finishes, the next is not started */
static volatile bool s_paused = false, s_idle = false;
static uint32_t s_block_start; /* source offset where the playing block began */
static uint32_t s_block_end, s_block_pit; /* its end, and when it started */

extern void isr_sb16(void);

// DSP / mixer

static bool dsp_reset(void) {
    outb(DSP_RESET, 1);
    for (volatile int i = 0; i < 300; i++)
        ; /* >= 3 us */
    outb(DSP_RESET, 0);
    for (volatile uint32_t i = 0; i < 0x10000u; i++)
        if ((inb(DSP_READ_STATUS) & 0x80) && inb(DSP_READ) == 0xAA)
            return true;
    return false;
}

static bool dsp_write(uint8_t val) {
    for (uint32_t i = 0; i < DSP_WRITE_TIMEOUT; i++)
        if (!(inb(DSP_WRITE) & 0x80)) {
            outb(DSP_WRITE, val);
            return true;
        }
    return false;
}

static int dsp_read_byte(void) {
    for (uint32_t i = 0; i < DSP_READ_TIMEOUT; i++)
        if (inb(DSP_READ_STATUS) & 0x80)
            return (int)(uint8_t)inb(DSP_READ);
    return -1;
}

static bool has_mixer(void) { return s_major >= 3; }

void mixer_write(uint8_t reg, uint8_t val) {
    if (!has_mixer())
        return; /* SB 1.x/2.0: no mixer chip */
    outb(MIXER_ADDR, reg);
    outb(MIXER_DATA, val);
}

static uint8_t mixer_read(uint8_t reg) {
    outb(MIXER_ADDR, reg);
    return inb(MIXER_DATA);
}

// DMA / IRQ

static void dma8_setup(uint32_t phys_addr, uint32_t byte_count) {
    uint8_t ch = (uint8_t)s_dma;
    outb(DMA8_MASK, 0x04 | ch); /* mask */
    outb(DMA8_CLEAR_FF, 0x00);
    outb(DMA8_MODE, 0x48 | ch); /* single, memory -> card */
    outb(ch * 2, (uint8_t)(phys_addr & 0xFF));
    outb(ch * 2, (uint8_t)((phys_addr >> 8) & 0xFF));
    outb(DMA8_PAGE[ch], (uint8_t)((phys_addr >> 16) & 0xFF));
    uint16_t count = (uint16_t)(byte_count - 1);
    outb(ch * 2 + 1, (uint8_t)(count & 0xFF));
    outb(ch * 2 + 1, (uint8_t)((count >> 8) & 0xFF));
    outb(DMA8_MASK, ch); /* unmask */
}

static void irq_on(void) { idt_set_irq(s_irq, isr_sb16); }
static void irq_off(void) { idt_mask_irq(s_irq); }
static void eoi(void) {
    if (s_irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static uint16_t read_irr(void) {
    outb(0x20, 0x0A);
    outb(0xA0, 0x0A);
    return (uint16_t)(inb(0x20) | (inb(0xA0) << 8));
}

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f)::"memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    __asm__ volatile("pushl %0; popfl" ::"r"(f) : "memory", "cc");
}

/* Wait (interrupts off) for a new PIC request line; returns its mask. The
 * DSP holds the line until DSP_ACK_8 is read, so IRR keeps the bit. */
static uint16_t wait_new_irq(uint16_t before, uint32_t spins) {
    while (spins--) {
        uint16_t rose = (uint16_t)(read_irr() & ~before) & ~(uint16_t)0x0007;
        if (rose)
            return rose;
    }
    return 0;
}

static int mask_to_irq(uint16_t m) {
    for (int i = 3; i < 16; i++)
        if (m == (1u << i))
            return i;
    return -1;
}

/* Pre-SB16: which line is the card jumpered to? */
static int detect_irq(void) {
    uint32_t fl = irq_save();
    uint16_t before = read_irr();
    int irq = -1;
    if (dsp_write(DSP_CMD_RAISE_IRQ))
        irq = mask_to_irq(wait_new_irq(before, 0x20000));
    (void)inb(DSP_ACK_8);
    irq_restore(fl);
    return irq;
}

/* Does an 8-sample transfer on DMA channel `ch` complete (-> IRQ)? */
static bool dma_works(int ch) {
    uint32_t fl = irq_save();
    s_dma = ch;
    memset(s_dma_buf, 0x80, 16);
    uint16_t before = read_irr();
    dma8_setup(DMA_BUF_PHYS, 8);
    dsp_write(DSP_CMD_TIME_CONST);
    dsp_write(256 - 1000000 / 22050);
    dsp_write(DSP_CMD_DMA8_SINGLE);
    dsp_write(7);
    dsp_write(0);
    bool ok = (wait_new_irq(before, 0x40000) & (1u << s_irq)) != 0;
    (void)inb(DSP_ACK_8);
    outb(DMA8_MASK, 0x04 | ch);
    irq_restore(fl);
    if (!ok)
        dsp_reset(); /* a DSP stuck waiting for DMA needs a reset */
    return ok;
}

// playback

/* Copy the next block (decimated by s_step) into the DMA buffer; returns
 * samples written, 0 at the end. */
static uint32_t fill_buffer(void) {
    uint32_t n = 0;
    while (n < DMA_BUF_SIZE && s_src_offset < s_src_len) {
        s_dma_buf[n++] = s_src_data[s_src_offset];
        s_src_offset += s_step;
    }
    if (n && n < DMA_BUF_SIZE)
        memset(s_dma_buf + n, 0x80, DMA_BUF_SIZE - n); /* silence */
    return n;
}

static bool dsp_program_chunk(uint32_t count_minus_1) {
    if (s_major >= 4) {
        return dsp_write(DSP_CMD_SET_RATE) &&
               dsp_write((uint8_t)(s_rate >> 8)) &&
               dsp_write((uint8_t)(s_rate & 0xFF)) &&
               dsp_write(DSP_CMD_PLAY_8BIT) && dsp_write(0x00) &&
               dsp_write((uint8_t)(count_minus_1 & 0xFF)) &&
               dsp_write((uint8_t)((count_minus_1 >> 8) & 0xFF));
    }
    uint32_t tc = 256 - 1000000 / s_rate;
    return dsp_write(DSP_CMD_TIME_CONST) && dsp_write((uint8_t)tc) &&
           dsp_write(DSP_CMD_DMA8_SINGLE) &&
           dsp_write((uint8_t)(count_minus_1 & 0xFF)) &&
           dsp_write((uint8_t)((count_minus_1 >> 8) & 0xFF));
}

static bool start_block(void) {
    s_block_start = s_src_offset;
    s_block_pit = pit_ticks;
    uint32_t n = fill_buffer();
    s_block_end = s_src_offset;
    if (!n && s_looping && s_src_data) {
        s_src_offset = 0;
        n = fill_buffer();
    }
    if (!n)
        return false;
    dma8_setup(DMA_BUF_PHYS, n);
    return dsp_program_chunk(n - 1);
}

void sb16_irq_handler(void) {
    if (s_detected) {
        (void)inb(DSP_ACK_8);
        if (s_playing && s_paused) {
            s_idle = true; /* resume starts the next block */
        } else if (s_playing && !start_block()) {
            s_playing = false;
            s_src_data = NULL;
        }
    }
    eoi();
}

// public API

bool sb16_init(void) {
    static const uint16_t ports[] = {0x220, 0x240, 0x260, 0x280};
    for (int i = 0; i < 4 && !s_detected; i++) {
        s_port = ports[i];
        if (!dsp_reset() || !dsp_write(DSP_CMD_GET_VERSION))
            continue;
        int major = dsp_read_byte(), minor = dsp_read_byte();
        if (major < 1 || minor < 0 || major > 9)
            continue;
        s_major = (uint8_t)major;
        s_minor = (uint8_t)minor;
        s_detected = true;
    }
    if (!s_detected)
        return false;

    if (!s_dma_buf) {
        uint8_t *raw = (uint8_t *)kmalloc(DMA_BUF_SIZE * 2);
        uint32_t a =
            ((uint32_t)raw + DMA_BUF_SIZE - 1) & ~(uint32_t)(DMA_BUF_SIZE - 1);
        if (raw && a + DMA_BUF_SIZE <= 0x1000000)
            s_dma_buf = (uint8_t *)a; /* kept for the OS lifetime */
        else if (raw)
            kfree(raw); /* above 16 MB: ISA DMA can't reach it */
    }

    if (s_major >= 4) {
        /* SB16: jumper-less, the mixer says where it is */
        uint8_t irqs = mixer_read(0x80), dmas = mixer_read(0x81);
        s_irq = (irqs & 2)   ? 5
                : (irqs & 4) ? 7
                : (irqs & 8) ? 10
                : (irqs & 1) ? 9
                             : 5;
        s_dma = (dmas & 2) ? 1 : (dmas & 8) ? 3 : (dmas & 1) ? 0 : 1;
    } else {
        int irq = detect_irq();
        s_irq = irq > 0 ? irq : 5;
        if (s_dma_buf && irq > 0) {
            static const int chans[3] = {1, 3, 0};
            bool ok = false;
            for (int i = 0; i < 3 && !ok; i++)
                ok = dma_works(chans[i]);
            if (!ok)
                s_dma = 1; /* nearly every card shipped on DMA 1 */
        }
    }

    dsp_write(DSP_CMD_SPEAKER_ON);
    mixer_write(MIXER_MASTER_VOL, 0xFF);
    mixer_write(MIXER_FM_VOL, 0xFF);
    mixer_write(MIXER_DAC_VOL, 0xFF);
    mixer_write(MIXER_LINE_VOL, 0xFF);
    mixer_write(MIXER_CD_VOL, 0xFF);
    return true;
}

bool sb16_detected(void) { return s_detected; }
int sb_dsp_version(void) { return s_detected ? (s_major << 8) | s_minor : 0; }
int sb_irq(void) { return s_irq; }
int sb_dma(void) { return s_dma; }
uint16_t sb_port(void) { return s_port; }

const char *sb_model_name(void) {
    if (!s_detected)
        return "none";
    switch (s_major) {
    case 1:
        return "Sound Blaster 1.x";
    case 2:
        return "Sound Blaster 2.0";
    case 3:
        return "Sound Blaster Pro";
    default:
        return "Sound Blaster 16";
    }
}

uint32_t sb_max_rate(void) { return s_major >= 4 ? 44100 : 22050; }

void sb16_set_volume(uint8_t left, uint8_t right) {
    if (!s_detected)
        return;
    uint8_t val = (uint8_t)((left & 0xF0) | (right >> 4));
    mixer_write(MIXER_DAC_VOL, val);
    mixer_write(MIXER_MASTER_VOL, val);
}

void sb16_play_pcm(const uint8_t *data, uint32_t len, uint32_t sample_rate,
                   bool stereo) {
    if (!s_detected || !s_dma_buf || !data || len == 0 || !sample_rate)
        return;
    (void)stereo; /* callers pass mono */
    sb16_stop();

    /* Faster than the card can go: play every s_step-th sample */
    s_step = (sample_rate + sb_max_rate() - 1) / sb_max_rate();
    s_rate = sample_rate / s_step;
    if (s_rate < 4000)
        s_rate = 4000;
    s_src_data = data;
    s_src_len = len;
    s_src_offset = 0;
    s_looping = false;
    s_paused = s_idle = false;
    s_playing = true;
    irq_on();
    if (!start_block()) {
        s_playing = false;
        s_src_data = NULL;
        irq_off();
    }
}

void sb16_set_loop(bool loop) { s_looping = loop; }

/* Pause: the card finishes the block it has (a fraction of a second) and
 * the next one waits. Resume starts it. */
void sb16_pause(bool pause) {
    if (!s_detected || !s_playing)
        return;
    __asm__ volatile("cli");
    s_paused = pause;
    bool restart = !pause && s_idle;
    s_idle = false;
    __asm__ volatile("sti");
    if (restart && !start_block()) {
        s_playing = false;
        s_src_data = NULL;
        irq_off();
    }
}

bool sb16_paused(void) { return s_playing && s_paused; }

/* where in the source the sound coming out is: the block being played,
 * plus the time since it started (paused: where it stopped) */
uint32_t sb16_position(void) {
    if (!s_playing)
        return 0;
    if (s_idle)
        return s_block_end;
    uint32_t ms = pit_ticks - s_block_pit;
    uint32_t p = s_block_start + ms * s_rate / 1000 * s_step;
    return p < s_block_end ? p : s_block_end;
}

/* the next block starts at `offset` (source samples) */
void sb16_seek(uint32_t offset) {
    if (!s_playing)
        return;
    __asm__ volatile("cli");
    s_src_offset = offset < s_src_len ? offset : s_src_len;
    __asm__ volatile("sti");
}

void sb16_stop(void) {
    if (!s_detected)
        return;
    s_playing = false;
    dsp_write(DSP_CMD_HALT_8);
    irq_off();
    s_src_data = NULL;
}

bool sb16_is_playing(void) { return s_playing; }

void sb16_unmute_fm(void) {
    if (!s_detected || s_major < 4)
        return; /* the SB Pro FM level is 0x26, set at init */
    mixer_write(MIXER_FM_VOL_LEFT, 0xFF);
    mixer_write(MIXER_FM_VOL_RIGHT, 0xFF);
}
