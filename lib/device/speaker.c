#include "lib/device/speaker.h"
#include "lib/core.h"
#include "lib/time.h"

#define PIT_CHANNEL2   0x42
#define PIT_CMD        0x43
#define SPEAKER_PORT   0x61

#define PIT_BASE_HZ    1193182

static bool     s_playing   = false;
static uint32_t s_stop_tick = 0;

void speaker_init(void) {
    speaker_tone_stop();
}

void speaker_tone_start(uint32_t frequency_hz) {
    if (frequency_hz == 0) { speaker_tone_stop(); return; }

    uint32_t divisor = PIT_BASE_HZ / frequency_hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    if (divisor < 1)      divisor = 1;

    outb(PIT_CMD, 0xB6);
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t val = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, val | 0x03);
}

void speaker_tone_stop(void) {
    uint8_t val = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, val & ~0x03);
    s_playing = false;
}

void speaker_beep(uint32_t frequency_hz, uint32_t duration_ms) {
    speaker_tone_start(frequency_hz);
    sleep_ms(duration_ms);
    speaker_tone_stop();
}

void speaker_beep_async(uint32_t frequency_hz, uint32_t duration_ms) {
    speaker_tone_start(frequency_hz);
    s_playing   = true;
    s_stop_tick = time_millis() + duration_ms;
}

void speaker_update(void) {
    if (s_playing && time_millis() >= s_stop_tick)
        speaker_tone_stop();
}
