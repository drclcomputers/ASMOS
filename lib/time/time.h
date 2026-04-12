#ifndef TIME_H
#define TIME_H

#include "lib/core.h"

typedef struct {
    uint32_t hours;
    uint32_t minutes;
    uint32_t seconds;
} time_hms_t;

typedef struct {
    uint32_t year;
    uint8_t  month;
    uint8_t  day;
} time_date_t;

typedef struct {
    uint32_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hours;
    uint8_t  minutes;
    uint8_t  seconds;
} time_full_t;

uint32_t    time_seconds(void);
uint32_t    time_millis(void);
uint32_t    pit_ticks_func(void);

void        sleep_ms(uint32_t ms);
void        sleep_s(uint32_t seconds);

time_hms_t  uptime_hms(void);
time_hms_t  time_to_hms(uint32_t total_seconds);
time_date_t time_to_date(uint32_t total_seconds);
time_full_t time_to_full(uint32_t total_seconds);

time_full_t time_rtc(void);
uint16_t    time_from_rtc(void);
uint16_t    date_from_rtc(void);
time_full_t time_rtc_local(void);

#endif
