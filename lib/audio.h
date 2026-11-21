#ifndef LIB_AUDIO_H
#define LIB_AUDIO_H

#include "lib/core.h"

/*
 * A codec table in lib/audio.c detects and plays supported formats, one at
 * a time. WAV supports PCM 8/16-bit mono/stereo; MIDI type 0/1 uses OPL2.
 * Call audio_update() every frame for playback.
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
