#ifndef WAV_H
#define WAV_H

#include "lib/core.h"

/* .WAV playback on the Sound Blaster. Accepts PCM 8/16-bit, mono/stereo,
 * any rate; converted to 8-bit mono (what every Sound Blaster can play).
 * Files up to WAV_MAX_BYTES. */

#define WAV_MAX_BYTES (1024u * 1024u)

/* Load + start playing. NULL on success, else a short reason. */
const char *wav_play_file(const char *path, bool loop);
void wav_stop(void);
bool wav_is_playing(void);

/* Parse a WAV image in memory and convert it in place to 8-bit unsigned
 * mono. Returns NULL on success (sets *pcm, *len, *rate). */
const char *wav_decode(uint8_t *file, uint32_t size, uint8_t **pcm,
                       uint32_t *len, uint32_t *rate);

#endif
