#ifndef SB16_H
#define SB16_H

#include "lib/core.h"

/* Whole Sound Blaster family (DSP 1.x - 4.x); names kept as sb16_* for
 * the existing callers. 8-bit mono PCM. */

#define SB16_DEFAULT_PORT 0x220

void mixer_write(uint8_t reg, uint8_t val);

bool sb16_init(void);
bool sb16_detected(void);

void sb16_set_volume(uint8_t left, uint8_t right);

void sb16_play_pcm(const uint8_t *data, uint32_t len, uint32_t sample_rate,
                   bool stereo);
void sb16_stop(void);
bool sb16_is_playing(void);
void sb16_unmute_fm(void);
void sb16_set_loop(bool loop);
void sb16_pause(bool pause); /* stops after the block in flight */
bool sb16_paused(void);
uint32_t sb16_position(void);    /* source sample being played */
void sb16_seek(uint32_t offset); /* next block from there */

const char *sb_model_name(void); /* "Sound Blaster Pro", ... */
int sb_dsp_version(void);        /* major << 8 | minor, 0 = none */
uint16_t sb_port(void);
int sb_irq(void);
int sb_dma(void);
uint32_t sb_max_rate(void); /* 22050 before the SB16, else 44100 */

#endif
