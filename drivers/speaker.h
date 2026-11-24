// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef SPEAKER_H
#define SPEAKER_H

#include "lib/core.h"

void speaker_init(void);
void speaker_beep(uint32_t frequency_hz, uint32_t duration_ms);
void speaker_tone_start(uint32_t frequency_hz);
void speaker_tone_stop(void);
void speaker_beep_async(uint32_t frequency_hz, uint32_t duration_ms);
void speaker_update(void);

#endif
