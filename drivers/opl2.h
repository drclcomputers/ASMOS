#ifndef OPL2_H
#define OPL2_H

#include "lib/core.h"

// OPL2 (YM3812) FM Synthesizer — built into every SB16.
//
// The OPL2 has 9 channels.  Each channel has two operators (carrier +
// modulator). We expose a simple API:
//   opl2_init()              — reset and detect the chip
//   opl2_note_on(ch, midi_note, volume)   — play a note (0=C-1 … 127=G9)
//   opl2_note_off(ch)        — release a note
//   opl2_set_instrument(ch, &inst) — load a patch
//   opl2_update()            — advance the software sequencer (call each frame)
//
// For MIDI files, use the midi_player_* API below which parses a Type-0/1
// MIDI byte stream you load into memory and drives opl2_note_on/off.

#define OPL2_BASE_PORT 0x388
#define OPL2_CHANNELS 9

// Instrument patch (two-operator FM)
typedef struct {
    uint8_t mod_avekm;  // reg 0x20+op  — AM/VIB/EG/KSR/MULT
    uint8_t mod_ksl_tl; // reg 0x40+op  — KSL/Total Level
    uint8_t mod_ar_dr;  // reg 0x60+op  — Attack/Decay
    uint8_t mod_sl_rr;  // reg 0x80+op  — Sustain/Release
    uint8_t mod_wave;   // reg 0xE0+op  — Waveform select

    uint8_t car_avekm;
    uint8_t car_ksl_tl;
    uint8_t car_ar_dr;
    uint8_t car_sl_rr;
    uint8_t car_wave;

    uint8_t fb_conn;
} opl2_instrument_t;

// General MIDI instrument table
extern const opl2_instrument_t opl2_gm_patches[128];

// OPL2 driver
bool opl2_init(void);
bool opl2_detected(void);
void opl2_reset(void);
void opl2_write(uint8_t reg, uint8_t val);
void opl2_set_instrument(uint8_t ch, const opl2_instrument_t *inst);
void opl2_note_on(uint8_t ch, uint8_t midi_note, uint8_t volume);
void opl2_note_off(uint8_t ch);
void opl2_all_notes_off(void);
void opl2_set_master_volume(uint8_t vol_0_to_100);

// Software MIDI sequencer
bool midi_player_load(const uint8_t *midi_data, uint32_t len);
void midi_player_play(bool loop);
void midi_player_stop(void);
void midi_player_pause(void);
bool midi_player_is_playing(void);
void midi_player_set_loop(bool loop);
void midi_player_update(void);

#endif
