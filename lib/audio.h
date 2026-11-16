#ifndef LIB_AUDIO_H
#define LIB_AUDIO_H

#include "lib/core.h"

/*
 * One sound at a time, whatever its format. Each format is a codec in a
 * table (lib/audio.c): it recognises its files by their first bytes and
 * plays them; adding a format (VOC, MOD...) means adding a codec there.
 *
 *   WAV   PCM 8/16-bit, mono/stereo, any rate, on the Sound Blaster
 *   MIDI  type 0/1 .MID through the OPL2 FM synth
 *
 * audio_update() must run every frame (the scheduler and the text shell
 * call it): MIDI is played from there.
 */

enum { AUDIO_EMPTY, AUDIO_STOPPED, AUDIO_PLAYING, AUDIO_PAUSED };

/* load a file (stops what was playing); NULL = fine, else why not */
const char *audio_open(const char *path);
void audio_close(void);

void audio_play(void);  /* from the start, or go on after a pause */
void audio_pause(void); /* toggles */
void audio_stop(void);  /* back to the start */
void audio_set_loop(bool loop);
bool audio_seek_ms(uint32_t ms); /* false: this format can't */
bool audio_can_seek(void);

int audio_state(void);
bool audio_finished(void); /* it ran to the end: true once, then false */
uint32_t audio_pos_ms(void);
uint32_t audio_len_ms(void);
const char *audio_codec_name(void); /* "WAV", "MIDI", "" */
const char *audio_info(void);       /* "22050 Hz, 8-bit mono" */
const char *audio_file(void);       /* the path loaded */
/* sound level now, 0..255 (WAV: from the samples; MIDI: 0) */
int audio_level(void);
/* file name extensions the codecs know: "WAV", "MID", ..., NULL */
const char *const *audio_extensions(void);

void audio_update(void);

#endif
