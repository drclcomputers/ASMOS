#ifndef SPEAKER_H
#define SPEAKER_H

#include "lib/types.h"

void speaker_init(void);
void speaker_beep(uint32_t frequency_hz, uint32_t duration_ms);
void speaker_tone_start(uint32_t frequency_hz);
void speaker_tone_stop(void);
void speaker_beep_async(uint32_t frequency_hz, uint32_t duration_ms);
void speaker_update(void);

#endif
