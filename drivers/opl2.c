#include "drivers/opl2.h"
#include "lib/core.h"
#include "lib/memory.h"

void opl2_reset(void);
void opl2_all_notes_off(void);
void opl2_note_off(uint8_t ch);
void midi_player_stop(void);

static uint16_t s_opl_port = 0;
static bool s_detected = false;
uint8_t s_master_vol = 100;

// Register I/O
// OPL2 requires >=3.3 µs between address write and data write,
// and >=23 µs after the data write before the next address write.
// We approximate both with port-read busy-loops (each ~1 µs on ISA).

static inline void opl_delay_short(void) {
    inb(0x80);
    inb(0x80);
    inb(0x80);
    inb(0x80);
}

static inline void opl_delay_long(void) {
    for (int i = 0; i < 25; i++)
        inb(0x80);
}

static inline void opl_write(uint8_t reg, uint8_t val) {
    outb(s_opl_port, reg);
    opl_delay_short();
    outb(s_opl_port + 1, val);
    opl_delay_long();
}

// Detection — standard OPL2 timer test (from Adlib FAQ)

static bool detect_port(uint16_t port) {
    // Reset both timers
    outb(port, 0x04);
    for (int i = 0; i < 6; i++)
        inb(0x80);
    outb(port + 1, 0x60);
    for (int i = 0; i < 35; i++)
        inb(0x80);

    outb(port, 0x04);
    for (int i = 0; i < 6; i++)
        inb(0x80);
    outb(port + 1, 0x80);
    for (int i = 0; i < 35; i++)
        inb(0x80);
    uint8_t s1 = inb(port);
    ;

    // Start timer 1 at 0xFF → overflows in ~80 µs
    outb(port, 0x02);
    for (int i = 0; i < 6; i++)
        inb(0x80);
    outb(port + 1, 0xFF);
    for (int i = 0; i < 35; i++)
        inb(0x80);

    outb(port, 0x04);
    for (int i = 0; i < 6; i++)
        inb(0x80);
    outb(port + 1, 0x21);
    for (int i = 0; i < 35; i++)
        inb(0x80);

    uint8_t s2 = 0;
    for (volatile int i = 0; i < 2000; i++) {
        s2 = inb(port);
        if ((s2 & 0xE0) == 0xC0)
            break;
    }

    // Reset again
    outb(port, 0x04);
    for (int i = 0; i < 6; i++)
        inb(0x80);
    outb(port + 1, 0x60);
    for (int i = 0; i < 35; i++)
        inb(0x80);
    outb(port, 0x04);
    for (int i = 0; i < 6; i++)
        inb(0x80);
    outb(port + 1, 0x80);
    for (int i = 0; i < 35; i++)
        inb(0x80);

    return (s1 & 0xE0) == 0x00 && (s2 & 0xE0) == 0xC0;
}

bool opl2_init(void) {
    static const uint16_t ports[] = {0x228, 0x388, 0x248};
    for (int i = 0; i < 3; i++) {
        if (detect_port(ports[i])) {
            s_opl_port = ports[i];
            s_detected = true;
            s_master_vol = 100;
            opl2_reset();
            return true;
        }
    }
    return false;
}

bool opl2_detected(void) { return s_detected; }

void opl2_write(uint8_t reg, uint8_t val) {
    if (s_opl_port)
        opl_write(reg, val);
}

void opl2_reset(void) {
    for (int r = 0x01; r <= 0xF5; r++)
        opl_write((uint8_t)r, 0x00);
    opl_write(0x01, 0x20);
    opl_write(0xBD, 0x00);
    for (int i = 0; i < 9; i++)
        opl_write(0xC0 + i, 0x30);
    for (volatile int i = 0; i < 1000; i++)
        inb(0x80);
}

// Operator offsets for each of the 9 channels.
// Index [ch][0] = modulator slot offset, [ch][1] = carrier slot offset.

static const uint8_t s_op[9][2] = {
    {0x00, 0x03}, {0x01, 0x04}, {0x02, 0x05}, {0x08, 0x0B}, {0x09, 0x0C},
    {0x0A, 0x0D}, {0x10, 0x13}, {0x11, 0x14}, {0x12, 0x15},
};

// Instrument load
// We preserve the KSL bits from the patch's mod_ksl_tl when loading,
// but the TL field is overridden per-note in opl2_note_on().

void opl2_set_instrument(uint8_t ch, const opl2_instrument_t *inst) {
    if (ch >= OPL2_CHANNELS || !inst)
        return;
    uint8_t m = s_op[ch][0], c = s_op[ch][1];
    opl_write(0x20 + m, inst->mod_avekm);
    opl_write(0x40 + m, inst->mod_ksl_tl);
    opl_write(0x60 + m, inst->mod_ar_dr);
    opl_write(0x80 + m, inst->mod_sl_rr);
    opl_write(0xE0 + m, inst->mod_wave & 0x07);
    opl_write(0x20 + c, inst->car_avekm);
    opl_write(0x40 + c, inst->car_ksl_tl);
    opl_write(0x60 + c, inst->car_ar_dr);
    opl_write(0x80 + c, inst->car_sl_rr);
    opl_write(0xE0 + c, inst->car_wave & 0x07);
    opl_write(0xC0 + ch, inst->fb_conn | 0x30);
}

// Pitch — exact OPL2 F-number formula:
//   F = freq * 2^(20 - block) / 49716
// Base frequencies (mHz) for MIDI notes 0-11 in block 0, then shift up by
// block. We work in units of 0.001 Hz to stay in integer arithmetic.

static const uint32_t s_note_mhz[12] = {
    // C      C#     D      D#     E      F
    16352,
    17324,
    18354,
    19445,
    20602,
    21827,
    // F#     G      G#     A      A#     B
    23125,
    24500,
    25957,
    27500,
    29135,
    30868,
};

static void note_to_fnum(uint8_t note, uint16_t *fnum, uint8_t *block) {
    int b = (int)(note / 12);
    if (b < 0)
        b = 0;
    if (b > 7)
        b = 7;

    uint32_t freq_mhz = s_note_mhz[note % 12];
    uint32_t f = (freq_mhz * (1u << 20)) / 49716000u;

    // Clamp to 10 bits
    if (f > 0x3FF)
        f = 0x3FF;
    *fnum = (uint16_t)f;
    *block = (uint8_t)b;
}

// Volume — OPL2 carrier Total Level is attenuation in 0.75 dB steps.
// TL=0 → full volume, TL=63 → ~47 dB down (silent for practical purposes).
// MIDI velocity is perceptually logarithmic; we apply a square-law curve
// to approximate that on the linear-dB TL scale.

void opl2_set_master_volume(uint8_t v) {
    if (v > 100)
        v = 100;
    s_master_vol = v;
}

static uint8_t velocity_to_tl(uint8_t vel, uint8_t master,
                              uint8_t patch_tl_ksl) {
    uint32_t eff = (uint32_t)vel * master / 100u;
    if (eff > 127)
        eff = 127;

    uint8_t tl_vol = (uint8_t)(63u - (eff * 63u / 127u));

    uint8_t patch_tl = patch_tl_ksl & 0x3F;
    uint32_t tl = tl_vol + patch_tl;
    if (tl > 63)
        tl = 63;

    return (patch_tl_ksl & 0xC0) | (uint8_t)(tl & 0x3F);
}

// Shadow the B0 register so note_off can clear only the key-on bit
// without re-reading a write-only port.
static uint8_t s_b0[OPL2_CHANNELS];

void opl2_note_on(uint8_t ch, uint8_t note, uint8_t vel) {
    if (ch >= OPL2_CHANNELS)
        return;
    // Writing B0 with key-on=0 first ensures a clean re-trigger
    opl_write(0xB0 + ch, 0x00);

    uint8_t c = s_op[ch][1];
    // We need the patch's carrier KSL/TL to compute volume correctly.
    // The sequencer always calls opl2_set_instrument before opl2_note_on,
    // so we read it back from the shadow in the voice table (set by sequencer).
    // Here we just apply velocity directly — the sequencer passes the patch
    // ptr. Volume is applied to carrier TL only (standard OPL2 FM practice).
    uint32_t eff = (uint32_t)vel * s_master_vol / 100u;
    if (eff > 127)
        eff = 127;
    uint32_t eff_sq = eff * eff;
    uint8_t tl = (uint8_t)(63u - (eff_sq * 63u / 16129u));
    opl_write(0x40 + c, tl & 0x3F);

    uint16_t f;
    uint8_t b;
    note_to_fnum(note, &f, &b);
    opl_write(0xA0 + ch, (uint8_t)(f & 0xFF));
    uint8_t b0val = (uint8_t)(0x20u | ((b & 7u) << 2) | ((f >> 8) & 3u));
    s_b0[ch] = b0val;
    opl_write(0xB0 + ch, b0val);
}

void opl2_note_off(uint8_t ch) {
    if (ch >= OPL2_CHANNELS)
        return;
    opl_write(0xB0 + ch, s_b0[ch] & ~0x20u);
}

void opl2_all_notes_off(void) {
    for (uint8_t c = 0; c < OPL2_CHANNELS; c++) {
        // Fastest possible release before clearing key-on
        opl_write(0x80 + s_op[c][0], 0xFF);
        opl_write(0x80 + s_op[c][1], 0xFF);
        opl_write(0xB0 + c, 0x00);
        s_b0[c] = 0x00;
    }
}

// We need to fix note_on to use the shadow too:
// (patch up note_on_ex to write through shadow)

// GM patch table — all 128 programs
// Format: mod_avekm, mod_ksl_tl, mod_ar_dr, mod_sl_rr, mod_wave,
//         car_avekm, car_ksl_tl, car_ar_dr, car_sl_rr, car_wave, fb_conn

const opl2_instrument_t opl2_gm_patches[128] = {
    {0x01, 0x8F, 0xF2, 0x44, 0x00, 0x01, 0x06, 0xF2, 0x54, 0x00,
     0x06}, //   0 Acoustic Grand Piano
    {0x01, 0x4F, 0xF2, 0x54, 0x00, 0x01, 0x06, 0xA5, 0x74, 0x00,
     0x06}, //   1 Bright Acoustic Piano
    {0x01, 0x8F, 0xF2, 0x44, 0x00, 0x01, 0x06, 0xF2, 0x54, 0x00,
     0x08}, //   2 Electric Grand Piano
    {0x01, 0x4F, 0xF2, 0x54, 0x00, 0x01, 0x06, 0xA5, 0x74, 0x00,
     0x08}, //   3 Honky-tonk Piano
    {0x01, 0x49, 0xF2, 0x55, 0x00, 0x01, 0x00, 0xC1, 0x56, 0x00,
     0x08}, //   4 Electric Piano 1
    {0x01, 0x49, 0xF2, 0x55, 0x00, 0x01, 0x00, 0xC1, 0x56, 0x00,
     0x06}, //   5 Electric Piano 2
    {0x07, 0x8F, 0xF1, 0x71, 0x01, 0x11, 0x00, 0xF2, 0xF1, 0x01,
     0x04}, //   6 Harpsichord
    {0x0E, 0x4F, 0xDA, 0x58, 0x00, 0x16, 0x00, 0xDA, 0x58, 0x00,
     0x04}, //   7 Clavinet
    {0x01, 0x40, 0xF2, 0x55, 0x01, 0x81, 0x00, 0x73, 0x67, 0x00,
     0x00}, //   8 Celesta
    {0x18, 0x40, 0xF2, 0x45, 0x01, 0x81, 0x00, 0xF5, 0x57, 0x00,
     0x00}, //   9 Glockenspiel
    {0x18, 0x40, 0xF6, 0x45, 0x00, 0x81, 0x00, 0x67, 0x56, 0x00,
     0x00}, //  10 Music Box
    {0x08, 0x4F, 0xF2, 0x53, 0x01, 0x01, 0x00, 0xF1, 0x73, 0x00,
     0x00}, //  11 Vibraphone
    {0x08, 0x46, 0xF2, 0x57, 0x00, 0x18, 0x00, 0xF1, 0x79, 0x00,
     0x00}, //  12 Marimba
    {0x08, 0x4A, 0xF2, 0x56, 0x00, 0x18, 0x00, 0xF1, 0x79, 0x00,
     0x00}, //  13 Xylophone
    {0x14, 0x40, 0xF2, 0x57, 0x00, 0x01, 0x00, 0xF1, 0x76, 0x01,
     0x00}, //  14 Tubular Bells
    {0x07, 0x8F, 0xF1, 0x71, 0x01, 0x11, 0x00, 0xF2, 0xF1, 0x01,
     0x06}, //  15 Dulcimer
    {0x86, 0x40, 0xF0, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00,
     0x0E}, //  16 Drawbar Organ
    {0x86, 0x40, 0x70, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00,
     0x0C}, //  17 Percussive Organ
    {0x86, 0x40, 0xF0, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00,
     0x08}, //  18 Rock Organ
    {0x23, 0x8F, 0xF2, 0x01, 0x00, 0x21, 0x00, 0xF2, 0x21, 0x00,
     0x04}, //  19 Church Organ
    {0x31, 0x8F, 0xF0, 0x02, 0x00, 0x31, 0x00, 0xF0, 0x02, 0x00,
     0x04}, //  20 Reed Organ
    {0xB1, 0x4F, 0xF2, 0x52, 0x00, 0x61, 0x00, 0xF3, 0x73, 0x00,
     0x06}, //  21 Accordion
    {0x30, 0x8F, 0xD2, 0x41, 0x00, 0x21, 0x00, 0xD1, 0x51, 0x00,
     0x08}, //  22 Harmonica
    {0xB1, 0x4F, 0xF2, 0x52, 0x00, 0x61, 0x00, 0xF3, 0x73, 0x00,
     0x08}, //  23 Tango Accordion
    {0x21, 0x49, 0x75, 0x55, 0x01, 0x22, 0x00, 0x75, 0x76, 0x00,
     0x04}, //  24 Nylon Guitar
    {0x21, 0x49, 0x75, 0x55, 0x01, 0x22, 0x00, 0x75, 0x76, 0x00,
     0x06}, //  25 Steel Guitar
    {0x21, 0x49, 0x75, 0x55, 0x01, 0x32, 0x00, 0x75, 0x76, 0x00,
     0x04}, //  26 Jazz Guitar
    {0x21, 0x49, 0x75, 0x55, 0x01, 0x22, 0x00, 0x75, 0x76, 0x00,
     0x08}, //  27 Clean Electric Guitar
    {0x21, 0x4B, 0x97, 0x15, 0x00, 0x22, 0x00, 0x87, 0x25, 0x00,
     0x06}, //  28 Muted Guitar
    {0x21, 0x4F, 0xF1, 0x52, 0x00, 0x22, 0x00, 0xF1, 0x72, 0x00,
     0x06}, //  29 Overdriven Guitar
    {0x21, 0x4F, 0xF1, 0x52, 0x00, 0x22, 0x00, 0xF1, 0x72, 0x00,
     0x08}, //  30 Distortion Guitar
    {0x21, 0x49, 0x75, 0x55, 0x01, 0x22, 0x00, 0x75, 0x76, 0x01,
     0x04}, //  31 Guitar Harmonics
    {0x31, 0x8A, 0xF1, 0x58, 0x00, 0x31, 0x00, 0xF1, 0x58, 0x00,
     0x0E}, //  32 Acoustic Bass
    {0x31, 0x8A, 0xF1, 0x58, 0x00, 0x31, 0x00, 0xF1, 0x58, 0x00,
     0x0C}, //  33 Electric Bass (finger)
    {0x31, 0x8E, 0xF1, 0x58, 0x00, 0x31, 0x00, 0xF1, 0x58, 0x00,
     0x0E}, //  34 Electric Bass (pick)
    {0x31, 0x8A, 0xF1, 0x58, 0x00, 0x31, 0x00, 0xF1, 0x58, 0x00,
     0x08}, //  35 Fretless Bass
    {0x31, 0x8A, 0xF4, 0x58, 0x00, 0x31, 0x00, 0xF4, 0x58, 0x00,
     0x0E}, //  36 Slap Bass 1
    {0x31, 0x8A, 0xF4, 0x58, 0x00, 0x31, 0x00, 0xF4, 0x58, 0x00,
     0x0C}, //  37 Slap Bass 2
    {0x31, 0x8A, 0xF1, 0x58, 0x00, 0x31, 0x00, 0xF1, 0x68, 0x00,
     0x0C}, //  38 Synth Bass 1
    {0x31, 0x8A, 0xF1, 0x58, 0x00, 0x31, 0x00, 0xF1, 0x68, 0x00,
     0x0E}, //  39 Synth Bass 2
    {0x31, 0x4F, 0xF3, 0x81, 0x00, 0x21, 0x00, 0xF3, 0x22, 0x00,
     0x0C}, //  40 Violin
    {0x31, 0x4F, 0xF3, 0x81, 0x00, 0x21, 0x00, 0xF3, 0x33, 0x00,
     0x0C}, //  41 Viola
    {0x71, 0x49, 0xF3, 0x81, 0x00, 0x21, 0x00, 0xF3, 0x44, 0x00,
     0x08}, //  42 Cello
    {0x71, 0x49, 0xF3, 0x81, 0x00, 0x21, 0x00, 0xF3, 0x55, 0x00,
     0x08}, //  43 Contrabass
    {0xF1, 0x40, 0xF1, 0x21, 0x02, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x06}, //  44 Tremolo Strings
    {0x02, 0x49, 0xF5, 0x55, 0x00, 0x01, 0x00, 0xF5, 0x75, 0x00,
     0x04}, //  45 Pizzicato Strings
    {0x21, 0x49, 0x75, 0x55, 0x01, 0x22, 0x00, 0x75, 0x76, 0x00,
     0x00}, //  46 Orchestral Harp
    {0x11, 0x40, 0xF1, 0x51, 0x00, 0x12, 0x00, 0xF2, 0x62, 0x00,
     0x08}, //  47 Timpani
    {0xF1, 0x40, 0xF1, 0x11, 0x02, 0xF1, 0x00, 0xF1, 0x11, 0x00,
     0x06}, //  48 String Ensemble 1
    {0xF1, 0x40, 0xF1, 0x11, 0x03, 0xF1, 0x00, 0xF1, 0x11, 0x00,
     0x06}, //  49 String Ensemble 2
    {0xF1, 0x40, 0xF1, 0x21, 0x02, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x06}, //  50 Synth Strings 1
    {0xF1, 0x40, 0xF1, 0x21, 0x03, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x06}, //  51 Synth Strings 2
    {0x71, 0x40, 0xF1, 0x31, 0x00, 0x51, 0x00, 0xF1, 0x41, 0x00,
     0x04}, //  52 Choir Aahs
    {0x71, 0x40, 0xF1, 0x31, 0x00, 0x51, 0x00, 0xF1, 0x51, 0x00,
     0x04}, //  53 Voice Oohs
    {0x71, 0x40, 0xF1, 0x31, 0x00, 0x51, 0x00, 0xF1, 0x61, 0x00,
     0x04}, //  54 Synth Voice
    {0x21, 0x48, 0x97, 0x43, 0x00, 0x22, 0x00, 0x88, 0x53, 0x00,
     0x06}, //  55 Orchestra Hit
    {0x21, 0x4F, 0xF7, 0x52, 0x00, 0x21, 0x00, 0xF7, 0x72, 0x00,
     0x04}, //  56 Trumpet
    {0x21, 0x4F, 0xF7, 0x52, 0x00, 0x21, 0x00, 0xF7, 0x72, 0x00,
     0x06}, //  57 Trombone
    {0x21, 0x4F, 0xF7, 0x52, 0x00, 0x21, 0x00, 0xF7, 0x72, 0x00,
     0x08}, //  58 Tuba
    {0x21, 0x4F, 0xF7, 0x52, 0x00, 0x21, 0x00, 0xF7, 0x72, 0x00,
     0x0A}, //  59 Muted Trumpet
    {0x21, 0x4F, 0xF7, 0x52, 0x00, 0x21, 0x00, 0xF7, 0x72, 0x00,
     0x0C}, //  60 French Horn
    {0xA1, 0x4F, 0xF5, 0x52, 0x00, 0x21, 0x00, 0xF6, 0x72, 0x00,
     0x04}, //  61 Brass Section
    {0xA1, 0x4F, 0xF5, 0x52, 0x00, 0x21, 0x00, 0xF6, 0x72, 0x00,
     0x06}, //  62 Synth Brass 1
    {0xA1, 0x4F, 0xF5, 0x52, 0x00, 0x21, 0x00, 0xF6, 0x72, 0x00,
     0x08}, //  63 Synth Brass 2
    {0x31, 0x4F, 0xF4, 0x52, 0x00, 0x21, 0x00, 0xF4, 0x62, 0x00,
     0x08}, //  64 Soprano Sax
    {0x31, 0x4F, 0xF4, 0x62, 0x00, 0x21, 0x00, 0xF4, 0x72, 0x00,
     0x08}, //  65 Alto Sax
    {0x31, 0x4F, 0xF4, 0x62, 0x00, 0x21, 0x00, 0xF4, 0x72, 0x00,
     0x0A}, //  66 Tenor Sax
    {0x31, 0x4F, 0xF4, 0x72, 0x00, 0x21, 0x00, 0xF4, 0x82, 0x00,
     0x08}, //  67 Baritone Sax
    {0x31, 0x4F, 0xF3, 0x32, 0x00, 0x21, 0x00, 0xF3, 0x52, 0x00,
     0x08}, //  68 Oboe
    {0x31, 0x4F, 0xF3, 0x42, 0x00, 0x21, 0x00, 0xF3, 0x62, 0x00,
     0x08}, //  69 English Horn
    {0x71, 0x4F, 0xF2, 0x52, 0x00, 0x21, 0x00, 0xF2, 0x62, 0x00,
     0x08}, //  70 Bassoon
    {0x31, 0x4F, 0xF3, 0x32, 0x00, 0x21, 0x00, 0xF3, 0x52, 0x00,
     0x0A}, //  71 Clarinet
    {0x01, 0x4F, 0xF2, 0x32, 0x01, 0x21, 0x00, 0xF2, 0x52, 0x01,
     0x04}, //  72 Piccolo
    {0x01, 0x4F, 0xF2, 0x32, 0x01, 0x21, 0x00, 0xF2, 0x52, 0x01,
     0x06}, //  73 Flute
    {0x01, 0x4F, 0xF2, 0x42, 0x01, 0x21, 0x00, 0xF2, 0x62, 0x01,
     0x06}, //  74 Recorder
    {0x01, 0x4F, 0xF2, 0x42, 0x01, 0x21, 0x00, 0xF2, 0x62, 0x01,
     0x04}, //  75 Pan Flute
    {0x01, 0x4F, 0xF2, 0x52, 0x01, 0x21, 0x00, 0xF2, 0x72, 0x01,
     0x06}, //  76 Blown Bottle
    {0x01, 0x4F, 0xF2, 0x52, 0x01, 0x21, 0x00, 0xF2, 0x72, 0x01,
     0x04}, //  77 Shakuhachi
    {0x01, 0x4F, 0xF2, 0x52, 0x00, 0x21, 0x00, 0xF2, 0x72, 0x00,
     0x04}, //  78 Whistle
    {0x01, 0x4F, 0xF2, 0x52, 0x00, 0x21, 0x00, 0xF2, 0x72, 0x00,
     0x06}, //  79 Ocarina
    {0x21, 0x4F, 0xF2, 0x03, 0x01, 0x21, 0x00, 0xF2, 0x03, 0x01,
     0x06}, //  80 Square Lead
    {0x21, 0x4F, 0xF2, 0x03, 0x00, 0x21, 0x00, 0xF2, 0x03, 0x00,
     0x06}, //  81 Sawtooth Lead
    {0x21, 0x4F, 0xF2, 0x03, 0x02, 0x21, 0x00, 0xF2, 0x03, 0x02,
     0x06}, //  82 Calliope Lead
    {0x21, 0x4F, 0xF2, 0x03, 0x00, 0x21, 0x00, 0xF2, 0x03, 0x00,
     0x0E}, //  83 Chiff Lead
    {0x21, 0x4F, 0xF2, 0x03, 0x01, 0x21, 0x00, 0xF2, 0x03, 0x00,
     0x0E}, //  84 Charang Lead
    {0x71, 0x4F, 0xF1, 0x31, 0x00, 0x51, 0x00, 0xF1, 0x41, 0x00,
     0x06}, //  85 Voice Lead
    {0x21, 0x4F, 0xF2, 0x03, 0x00, 0x21, 0x00, 0xF2, 0x03, 0x00,
     0x08}, //  86 Fifths Lead
    {0x21, 0x4F, 0xF2, 0x03, 0x00, 0x21, 0x00, 0xF2, 0x03, 0x00,
     0x0C}, //  87 Bass+Lead
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x04}, //  88 New Age Pad
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x06}, //  89 Warm Pad
    {0xF1, 0x40, 0xF1, 0x21, 0x01, 0xF1, 0x00, 0xF1, 0x21, 0x01,
     0x06}, //  90 Polysynth Pad
    {0x71, 0x40, 0xF1, 0x21, 0x00, 0x51, 0x00, 0xF1, 0x31, 0x00,
     0x04}, //  91 Choir Pad
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x08}, //  92 Bowed Pad
    {0xF1, 0x40, 0xF1, 0x21, 0x01, 0xF1, 0x00, 0xF1, 0x21, 0x01,
     0x08}, //  93 Metallic Pad
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x0A}, //  94 Halo Pad
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x0C}, //  95 Sweep Pad
    {0x21, 0x4F, 0xF2, 0x03, 0x01, 0x22, 0x00, 0xF2, 0x03, 0x00,
     0x06}, //  96 Rain FX
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x06}, //  97 Soundtrack FX
    {0x08, 0x4F, 0xF2, 0x53, 0x01, 0x01, 0x00, 0xF1, 0x73, 0x00,
     0x00}, //  98 Crystal FX
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x08}, //  99 Atmosphere FX
    {0x21, 0x4F, 0xF2, 0x03, 0x01, 0x21, 0x00, 0xF2, 0x03, 0x01,
     0x0A}, // 100 Brightness FX
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x0C}, // 101 Goblins FX
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x0E}, // 102 Echoes FX
    {0xF1, 0x40, 0xF1, 0x21, 0x00, 0xF1, 0x00, 0xF1, 0x21, 0x00,
     0x0A}, // 103 Sci-fi FX
    {0x21, 0x49, 0x75, 0x55, 0x00, 0x22, 0x00, 0x75, 0x76, 0x00,
     0x06}, // 104 Sitar
    {0x21, 0x49, 0x75, 0x55, 0x00, 0x22, 0x00, 0x75, 0x76, 0x00,
     0x04}, // 105 Banjo
    {0x21, 0x49, 0x75, 0x55, 0x00, 0x22, 0x00, 0x75, 0x76, 0x00,
     0x08}, // 106 Shamisen
    {0x21, 0x49, 0x75, 0x55, 0x01, 0x22, 0x00, 0x75, 0x76, 0x01,
     0x06}, // 107 Koto
    {0x08, 0x4A, 0xF2, 0x56, 0x00, 0x18, 0x00, 0xF1, 0x79, 0x00,
     0x00}, // 108 Kalimba
    {0x31, 0x8F, 0xF0, 0x02, 0x00, 0x31, 0x00, 0xF0, 0x02, 0x00,
     0x06}, // 109 Bagpipe
    {0x31, 0x4F, 0xF3, 0x81, 0x00, 0x21, 0x00, 0xF3, 0x22, 0x00,
     0x0E}, // 110 Fiddle
    {0x31, 0x4F, 0xF3, 0x32, 0x00, 0x21, 0x00, 0xF3, 0x52, 0x00,
     0x0C}, // 111 Shanai
    {0x14, 0x40, 0xF2, 0x57, 0x00, 0x01, 0x00, 0xF1, 0x76, 0x01,
     0x04}, // 112 Tinkle Bell
    {0x18, 0x40, 0xF2, 0x45, 0x00, 0x81, 0x00, 0xF5, 0x57, 0x00,
     0x04}, // 113 Agogo
    {0x08, 0x46, 0xF2, 0x57, 0x00, 0x18, 0x00, 0xF1, 0x79, 0x00,
     0x04}, // 114 Steel Drums
    {0x18, 0x4F, 0xF2, 0x77, 0x00, 0x11, 0x00, 0xF0, 0x97, 0x00,
     0x04}, // 115 Woodblock
    {0x18, 0x4F, 0xF2, 0x67, 0x00, 0x11, 0x00, 0xF0, 0x97, 0x00,
     0x04}, // 116 Taiko Drum
    {0x18, 0x4F, 0xF2, 0x57, 0x00, 0x11, 0x00, 0xF0, 0x97, 0x00,
     0x04}, // 117 Melodic Tom
    {0x18, 0x4F, 0xF2, 0x57, 0x00, 0x11, 0x00, 0xF0, 0x97, 0x00,
     0x06}, // 118 Synth Drum
    {0x01, 0x8F, 0x0F, 0xFF, 0x00, 0x01, 0x00, 0x0F, 0xFF, 0x00,
     0x08}, // 119 Reverse Cymbal
    {0x01, 0x4F, 0xD2, 0xB3, 0x00, 0x01, 0x00, 0xD3, 0xC3, 0x00,
     0x04}, // 120 Guitar Fret Noise
    {0x21, 0x4F, 0xF2, 0x03, 0x00, 0x22, 0x00, 0xF2, 0x03, 0x00,
     0x04}, // 121 Breath Noise
    {0xF1, 0x4F, 0xF2, 0x21, 0x00, 0xF1, 0x00, 0xF2, 0x31, 0x00,
     0x06}, // 122 Seashore
    {0x01, 0x4F, 0xF2, 0x32, 0x01, 0x21, 0x00, 0xF2, 0x52, 0x01,
     0x04}, // 123 Bird Tweet
    {0x21, 0x4F, 0xF2, 0x03, 0x01, 0x21, 0x00, 0xF2, 0x03, 0x01,
     0x08}, // 124 Telephone Ring
    {0x01, 0x8F, 0xF0, 0xA0, 0x00, 0x01, 0x00, 0xF0, 0xA0, 0x00,
     0x0E}, // 125 Helicopter
    {0xF1, 0x4F, 0xF2, 0x21, 0x00, 0xF1, 0x00, 0xF2, 0x31, 0x00,
     0x0E}, // 126 Applause
    {0x01, 0x8F, 0xF0, 0xA0, 0x00, 0x01, 0x00, 0xF0, 0xA0, 0x00,
     0x08} // 127 Gunshot
};

// ===========================================================================
// MIDI sequencer
// ===========================================================================

#define MAX_TRACKS 16
#define MAX_CHANNELS 16

// Per-voice slot: one active note on one OPL channel
typedef struct {
    int8_t opl_ch;
    uint8_t midi_ch;
    uint8_t note;
    uint8_t age;
    uint8_t prog;
} voice_t;

static voice_t s_voices[OPL2_CHANNELS];
static uint8_t s_age_ctr;

// Per MIDI channel state
static uint8_t s_prog[MAX_CHANNELS]; // current program (patch)
static int16_t s_bend[MAX_CHANNELS]; // pitch bend, -8192..+8191 (unused in
                                     // pitch calc but tracked)
static uint8_t s_expr[MAX_CHANNELS]; // expression CC11, 0-127

typedef struct {
    const uint8_t *pos, *end;
    uint32_t wait;
    bool done;
    uint8_t rs; // running status
} track_t;

static const uint8_t *s_midi_data;
static uint32_t s_midi_len;
static bool s_playing, s_looping, s_paused;
static uint16_t s_ppq;
static uint32_t s_tempo;
static uint32_t s_lpit, s_tacc;
static track_t s_tracks[MAX_TRACKS];
static int s_track_count;

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t vlq(const uint8_t **pp, const uint8_t *end) {
    uint32_t v = 0;
    while (*pp < end) {
        uint8_t b = *(*pp)++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            break;
    }
    return v;
}

static void voices_init(void) {
    for (int i = 0; i < OPL2_CHANNELS; i++) {
        s_voices[i].opl_ch = (int8_t)i;
        s_voices[i].midi_ch = 0xFF;
        s_voices[i].note = 0xFF;
        s_voices[i].age = 0;
        s_voices[i].prog = 0xFF;
    }
    s_age_ctr = 0;
}

static void seq_reset(void) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        s_prog[i] = 0;
        s_bend[i] = 0;
        s_expr[i] = 127;
    }
    s_tacc = 0;
    voices_init();
}

// Find a voice already playing (midi_ch, note) — for note-off matching
static int voice_find(uint8_t midi_ch, uint8_t note) {
    for (int i = 0; i < OPL2_CHANNELS; i++)
        if (s_voices[i].midi_ch == midi_ch && s_voices[i].note == note)
            return i;
    return -1;
}

// Allocate an OPL channel. Prefers free slots; steals oldest active note.
static int voice_alloc(void) {
    // Prefer a slot with no active note
    for (int i = 0; i < OPL2_CHANNELS; i++)
        if (s_voices[i].midi_ch == 0xFF)
            return i;

    // Steal the oldest voice
    int oldest = 0;
    for (int i = 1; i < OPL2_CHANNELS; i++)
        if ((uint8_t)(s_age_ctr - s_voices[i].age) >
            (uint8_t)(s_age_ctr - s_voices[oldest].age))
            oldest = i;

    opl_write(0xB0 + oldest, 0x00);
    s_b0[oldest] = 0x00;
    s_voices[oldest].prog = 0xFF;
    return oldest;
}

static void note_on_voice(uint8_t midi_ch, uint8_t note, uint8_t vel) {
    if (vel == 0) {
        // velocity-0 note-on acts as note-off
        int v = voice_find(midi_ch, note);
        if (v >= 0) {
            opl2_note_off((uint8_t)v);
            s_voices[v].midi_ch = 0xFF;
            s_voices[v].note = 0xFF;
        }
        return;
    }

    // Re-trigger if already sounding (same ch+note)
    int v = voice_find(midi_ch, note);
    if (v < 0)
        v = voice_alloc();

    uint8_t prog = s_prog[midi_ch] & 0x7F;
    const opl2_instrument_t *inst = &opl2_gm_patches[prog];
    if (s_voices[v].prog != prog) {
        opl2_set_instrument((uint8_t)v, inst);
        s_voices[v].prog = prog;
    }

    // Apply expression (CC11) on top of velocity
    uint32_t eff_vel = (uint32_t)vel * s_expr[midi_ch] / 127u;
    if (eff_vel > 127)
        eff_vel = 127;

    // Write B0 key-off first for clean retrigger, then set pitch and key-on
    opl_write(0xB0 + v, 0x00);

    uint8_t c = s_op[v][1];
    uint8_t ksl_tl =
        velocity_to_tl((uint8_t)eff_vel, s_master_vol, inst->car_ksl_tl);
    opl_write(0x40 + c, ksl_tl);

    uint16_t fnum;
    uint8_t block;
    note_to_fnum(note, &fnum, &block);

    int32_t bend_cents = (int32_t)s_bend[midi_ch] * 200 / 8192;
    int16_t fnum_bent = fnum;
    uint8_t block_bent = block;
    while (bend_cents >= 100) {
        fnum_bent =
            (int16_t)((uint32_t)fnum_bent * 1060u / 1000u); // * 2^(1/12)
        if (fnum_bent >= 1024) {
            fnum_bent >>= 1;
            block_bent++;
        }
        bend_cents -= 100;
    }
    while (bend_cents <= -100) {
        fnum_bent = (int16_t)((uint32_t)fnum_bent * 943u / 1000u); // / 2^(1/12)
        if (fnum_bent < 344 && block_bent > 0) {
            fnum_bent <<= 1;
            block_bent--;
        }
        bend_cents += 100;
    }
    if (block_bent > 7)
        block_bent = 7;
    if (block_bent < 0)
        block_bent = 0;
    // Clamp fnum to 0..0x3FF
    if (fnum_bent > 0x3FF)
        fnum_bent = 0x3FF;
    if (fnum_bent < 0x00)
        fnum_bent = 0x00;

    opl_write(0xA0 + v, (uint8_t)(fnum_bent & 0xFF));
    uint8_t b0val =
        (uint8_t)(0x20u | ((block_bent & 7u) << 2) | ((fnum_bent >> 8) & 3u));
    s_b0[v] = b0val;
    opl_write(0xB0 + v, b0val);

    s_voices[v].midi_ch = midi_ch;
    s_voices[v].note = note;
    s_voices[v].age = s_age_ctr++;
}

static void note_off_voice(uint8_t midi_ch, uint8_t note) {
    int v = voice_find(midi_ch, note);
    if (v < 0)
        return;
    opl_write(0xB0 + v, s_b0[v] & ~0x20u);
    s_voices[v].midi_ch = 0xFF;
    s_voices[v].note = 0xFF;
}

static void dispatch(uint8_t ch, uint8_t type, uint8_t b1, uint8_t b2) {
    if (ch == 9 && type != 0xC && type != 0xB)
        return; // skip GM percussion — no melodic OPL2 mapping

    switch (type) {
    case 0x8:
        note_off_voice(ch, b1);
        break;
    case 0x9:
        note_on_voice(ch, b1, b2);
        break;
    case 0xA: // aftertouch — ignore
        break;
    case 0xB:        // control change
        if (b1 == 7) // channel volume
            s_expr[ch] = b2;
        else if (b1 == 11) // expression
            s_expr[ch] = b2;
        else if (b1 == 120 || b1 == 123) { // all sound/notes off
            for (int i = 0; i < OPL2_CHANNELS; i++) {
                if (s_voices[i].midi_ch == ch) {
                    opl2_note_off((uint8_t)i);
                    s_voices[i].midi_ch = 0xFF;
                    s_voices[i].note = 0xFF;
                }
            }
        }
        break;
    case 0xC: // program change
        s_prog[ch] = b1 & 0x7F;
        break;
    case 0xE: // pitch bend — store for reference, not applied to running notes
        s_bend[ch] = (int16_t)(((uint16_t)b2 << 7 | b1) - 8192);
        break;
    }
}

static void track_read_delta(track_t *t) {
    if (t->done || t->pos >= t->end) {
        t->done = true;
        return;
    }
    t->wait = vlq(&t->pos, t->end);
}

static void track_process_event(track_t *t) {
    if (t->done || t->pos >= t->end) {
        t->done = true;
        return;
    }

    uint8_t b = *t->pos;
    uint8_t st;
    if (b & 0x80) {
        st = b;
        t->rs = b;
        t->pos++;
    } else {
        st = t->rs;
        if (!st) {
            t->pos++;
            track_read_delta(t);
            return;
        }
    }

    uint8_t type = (st >> 4) & 0xF;
    uint8_t ch = st & 0xF;

    if (type == 0xF) {
        if (st == 0xFF) {
            if (t->pos >= t->end) {
                t->done = true;
                return;
            }
            uint8_t meta = *t->pos++;
            uint32_t len = vlq(&t->pos, t->end);
            if (t->pos + len > t->end)
                len = (uint32_t)(t->end - t->pos);
            if (meta == 0x51 && len == 3) {
                s_tempo = ((uint32_t)t->pos[0] << 16) |
                          ((uint32_t)t->pos[1] << 8) | (uint32_t)t->pos[2];
                if (!s_tempo)
                    s_tempo = 500000;
            } else if (meta == 0x2F) {
                t->done = true;
                return;
            }
            t->pos += len;
        } else if (st == 0xF0 || st == 0xF7) {
            uint32_t len = vlq(&t->pos, t->end);
            if (t->pos + len > t->end)
                len = (uint32_t)(t->end - t->pos);
            t->pos += len;
        }
        track_read_delta(t);
        return;
    }

    uint8_t b1 = 0, b2 = 0;
    if (t->pos < t->end)
        b1 = *t->pos++;
    if (type != 0xC && type != 0xD && t->pos < t->end)
        b2 = *t->pos++;
    dispatch(ch, type, b1, b2);
    track_read_delta(t);
}

bool midi_player_load(const uint8_t *data, uint32_t len) {
    if (!data || len < 14)
        return false;
    if (data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd')
        return false;
    if (s_playing)
        midi_player_stop();

    uint16_t div = rd16(data + 12);
    if (div & 0x8000)
        return false; // SMPTE timecode not supported
    s_ppq = div ? div : 480;

    uint16_t ntracks = rd16(data + 10);
    if (ntracks > MAX_TRACKS)
        ntracks = MAX_TRACKS;

    s_track_count = 0;
    const uint8_t *p = data + 14, *end = data + len;
    while (p + 8 <= end && s_track_count < (int)ntracks) {
        if (p[0] != 'M' || p[1] != 'T' || p[2] != 'r' || p[3] != 'k')
            break;
        uint32_t chunk = rd32(p + 4);
        p += 8;
        if (p + chunk > end)
            chunk = (uint32_t)(end - p);
        track_t *t = &s_tracks[s_track_count++];
        t->pos = p;
        t->end = p + chunk;
        t->done = false;
        t->rs = 0;
        t->wait = 0;
        track_read_delta(t);
        p += chunk;
    }

    s_midi_data = data;
    s_midi_len = len;
    s_tempo = 500000;
    return s_track_count > 0;
}

void midi_player_play(bool loop) {
    if (!s_midi_data || !s_midi_len)
        return;
    midi_player_load(s_midi_data, s_midi_len);
    s_looping = loop;
    s_playing = true;
    s_paused = false;
    if (!s_master_vol)
        s_master_vol = 100;
    extern volatile uint32_t pit_ticks;
    s_lpit = pit_ticks;
    seq_reset();
    opl2_all_notes_off();
}

void midi_player_stop(void) {
    s_playing = false;
    s_paused = false;
    opl2_all_notes_off();
    opl2_reset();
    seq_reset();
}

void midi_player_pause(void) {
    if (!s_playing)
        return;
    s_paused = !s_paused;
    if (s_paused)
        opl2_all_notes_off();
}

void midi_player_set_loop(bool loop) { s_looping = loop; }
bool midi_player_is_playing(void) { return s_playing && !s_paused; }

void midi_player_update(void) {
    if (!s_playing || s_paused || !s_track_count)
        return;

    extern volatile uint32_t pit_ticks;
    uint32_t el = pit_ticks - s_lpit;
    s_lpit = pit_ticks;
    if (!el)
        return;

    if (!s_tempo)
        s_tempo = 500000;

    // Accumulate time in µs (PIT at ~1000 Hz → 1 tick ≈ 1000 µs)
    s_tacc += el * 1000u * s_ppq;
    uint32_t pulses = s_tacc / s_tempo;
    if (pulses > (uint32_t)s_ppq * 2)
        pulses = (uint32_t)s_ppq * 2;
    s_tacc %= s_tempo;

    if (!pulses)
        return;

    // Check if all tracks are done
    bool all_done = true;
    for (int i = 0; i < s_track_count; i++)
        if (!s_tracks[i].done) {
            all_done = false;
            break;
        }

    if (all_done) {
        if (s_looping && s_midi_data) {
            midi_player_load(s_midi_data, s_midi_len);
            midi_player_play(true);
        } else {
            midi_player_stop();
        }
        return;
    }

    for (int i = 0; i < s_track_count; i++) {
        track_t *t = &s_tracks[i];
        uint32_t rem = pulses;
        while (!t->done) {
            if (t->wait > rem) {
                t->wait -= rem;
                break;
            }
            rem -= t->wait;
            t->wait = 0;
            track_process_event(t);
        }
    }
}
