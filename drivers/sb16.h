#ifndef SB16_H
#define SB16_H

#include "lib/core.h"

#define SB16_DEFAULT_PORT 0x220
#define SB16_DMA_CHANNEL 1
#define SB16_IRQ 5

bool sb16_init(void);
bool sb16_detected(void);

void sb16_set_volume(uint8_t left, uint8_t right);

void sb16_play_pcm(const uint8_t *data, uint32_t len, uint32_t sample_rate,
                   bool stereo);
void sb16_stop(void);
bool sb16_is_playing(void);
void sb16_update(void);

#endif
