#include "lib/time.h"
#include "lib/io.h"
#include "interrupts/idt.h"
#include "config/runtime_config.h"

uint32_t time_seconds(void) {
    return pit_seconds;
}

uint32_t time_millis(void) {
    return pit_ticks * 10;
}

uint32_t pit_ticks_func(void) {
	return time_millis() / 10;
}

void sleep_ms(uint32_t ms) {
    uint32_t ticks_needed = (ms + 9) / 10;
    uint32_t target       = pit_ticks + ticks_needed;
    while (pit_ticks < target) {
        __asm__ volatile ("hlt");
    }
}

void sleep_s(uint32_t seconds) {
    sleep_ms(seconds * 1000);
}

time_hms_t time_to_hms(uint32_t total_seconds) {
    time_hms_t t;
    t.hours   =  total_seconds / 3600;
    t.minutes = (total_seconds % 3600) / 60;
    t.seconds =  total_seconds % 60;
    return t;
}

time_date_t time_to_date(uint32_t total_seconds) {
    uint32_t days = total_seconds / 86400;

    uint32_t era  = days / 146097;
    uint32_t doe  = days - era * 146097;
    uint32_t yoe  = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    uint32_t y    = yoe + era * 400;
    uint32_t doy  = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp   = (5 * doy + 2) / 153;
    uint32_t d    = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m    = mp < 10 ? mp + 3 : mp - 9;

    time_date_t date;
    date.year  = y + (m <= 2 ? 1 : 0);
    date.month = (uint8_t)m;
    date.day   = (uint8_t)d;
    return date;
}

time_full_t time_to_full(uint32_t total_seconds) {
    time_hms_t  hms  = time_to_hms(total_seconds);
    time_date_t date = time_to_date(total_seconds);

    time_full_t f;
    f.year    = date.year;
    f.month   = date.month;
    f.day     = date.day;
    f.hours   = (uint8_t)(hms.hours % 24);
    f.minutes = (uint8_t)hms.minutes;
    f.seconds = (uint8_t)hms.seconds;
    return f;
}

time_hms_t uptime_hms(void) {
    return time_to_hms(pit_seconds);
}

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

time_full_t time_rtc(void) {
    while (cmos_read(0x0A) & 0x80) {}

    uint8_t sec    = bcd_to_bin(cmos_read(0x00));
    uint8_t min    = bcd_to_bin(cmos_read(0x02));
    uint8_t hour   = bcd_to_bin(cmos_read(0x04));
    uint8_t day    = bcd_to_bin(cmos_read(0x07));
    uint8_t month  = bcd_to_bin(cmos_read(0x08));
    uint8_t year2  = bcd_to_bin(cmos_read(0x09));

    while (cmos_read(0x0A) & 0x80) {}
    uint8_t sec2   = bcd_to_bin(cmos_read(0x00));
    uint8_t min2   = bcd_to_bin(cmos_read(0x02));
    uint8_t hour2  = bcd_to_bin(cmos_read(0x04));
    uint8_t day2   = bcd_to_bin(cmos_read(0x07));
    uint8_t month2 = bcd_to_bin(cmos_read(0x08));
    uint8_t year22 = bcd_to_bin(cmos_read(0x09));

    if (sec != sec2 || min != min2) {
        sec = sec2; min = min2; hour = hour2;
        day = day2; month = month2; year2 = year22;
    }

    time_full_t t;
    t.seconds = sec;
    t.minutes = min;
    t.hours   = hour;
    t.day     = day;
    t.month   = month;
    t.year    = (year2 < 80) ? 2000 + year2 : 1980 + (year2 - 80);
    return t;
}

static int days_in_month(uint8_t month, uint32_t year) {
    int is_leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);

    if (month == 2) {
        return is_leap ? 29 : 28;
    }
    if (month == 4 || month == 6 || month == 9 || month == 11) {
        return 30;
    }
    return 31;
}

time_full_t time_rtc_local(void) {
    time_full_t t = time_rtc();

    int32_t hours = (int32_t)t.hours + (int8_t)g_cfg.timezone_offset;

    if (hours < 0) {
        hours += 24;

        if (t.day == 1) {
            if (t.month == 1) {
                t.month = 12;
                t.year--;
            } else {
                t.month--;
            }
            t.day = days_in_month(t.month, t.year);
        } else {
            t.day--;
        }
    } else if (hours >= 24) {
        hours -= 24;

        int max_day = days_in_month(t.month, t.year);
        if (t.day == max_day) {
            // Go to first day of next month
            if (t.month == 12) {
                t.month = 1;
                t.year++;
            } else {
                t.month++;
            }
            t.day = 1;
        } else {
            t.day++;
        }
    }

    t.hours = (uint8_t)hours;
    return t;
}

uint16_t time_from_rtc(void) {
    time_full_t t = time_rtc();
    return ((t.hours & 0x1F) << 11) | ((t.minutes & 0x3F) << 5) | ((t.seconds / 2) & 0x1F);
}

uint16_t date_from_rtc(void) {
    time_full_t t = time_rtc();
    uint16_t y = (t.year > 1980) ? (uint16_t)(t.year - 1980) : 0;
    return ((y & 0x7F) << 9) | ((t.month & 0x0F) << 5) | (t.day & 0x1F);
}
