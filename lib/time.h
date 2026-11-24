// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

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
    uint8_t month;
    uint8_t day;
} time_date_t;

typedef struct {
    uint32_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;
} time_full_t;

/* 1 kHz PIT counter, incremented in interrupts/idt.c. */
extern volatile uint32_t pit_ticks;

uint32_t time_seconds(void);
uint32_t time_millis(void);

void sleep_ms(uint32_t ms);
void sleep_s(uint32_t seconds);

time_hms_t uptime_hms(void);
time_hms_t time_to_hms(uint32_t total_seconds);
time_date_t time_to_date(uint32_t total_seconds);
time_full_t time_to_full(uint32_t total_seconds);

time_full_t time_rtc(void);
uint16_t time_from_rtc(void);
uint16_t date_from_rtc(void);
time_full_t time_rtc_local(void);

/* Set the clock from a local date and time (the Settings time zone is
 * taken off; the RTC keeps UTC). 1980..2099. false = invalid. */
bool time_set_local(const time_full_t *local);
bool time_rtc_set(const time_full_t *utc);

/* Calendar arithmetic, proleptic Gregorian, years 1..9999 */
bool time_is_leap(int y);
int time_days_in_month(int m, int y); /* m 1..12 */
bool time_date_valid(int y, int m, int d);
int32_t time_days_from_civil(int y, int m, int d); /* days since 1970-01-01 */
void time_civil_from_days(int32_t z, int *y, int *m, int *d);
int time_weekday(int y, int m, int d);     /* 0 = Monday .. 6 = Sunday */
int time_day_of_year(int y, int m, int d); /* 1..366 */
int time_iso_week(int y, int m, int d);    /* 1..53 */
const char *time_month_name(int m);        /* "January" */
const char *time_weekday_name(int wd);     /* "Monday" */

#endif
