#include "lib/time.h"
#include "config/runtime_config.h"
#include "interrupts/idt.h"
#include "lib/core.h"

uint32_t time_seconds(void) { return pit_seconds; }

uint32_t time_millis(void) { return pit_ticks; }

void sleep_ms(uint32_t ms) {
    uint32_t target = pit_ticks + ms;
    while (pit_ticks < target) {
        __asm__ volatile("hlt");
    }
}

void sleep_s(uint32_t seconds) { sleep_ms(seconds * 1000); }

time_hms_t time_to_hms(uint32_t total_seconds) {
    time_hms_t t;
    t.hours = total_seconds / 3600;
    t.minutes = (total_seconds % 3600) / 60;
    t.seconds = total_seconds % 60;
    return t;
}

time_date_t time_to_date(uint32_t total_seconds) {
    uint32_t days = total_seconds / 86400;

    uint32_t era = days / 146097;
    uint32_t doe = days - era * 146097;
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t y = yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;

    time_date_t date;
    date.year = y + (m <= 2 ? 1 : 0);
    date.month = (uint8_t)m;
    date.day = (uint8_t)d;
    return date;
}

time_full_t time_to_full(uint32_t total_seconds) {
    time_hms_t hms = time_to_hms(total_seconds);
    time_date_t date = time_to_date(total_seconds);

    time_full_t f;
    f.year = date.year;
    f.month = date.month;
    f.day = date.day;
    f.hours = (uint8_t)(hms.hours % 24);
    f.minutes = (uint8_t)hms.minutes;
    f.seconds = (uint8_t)hms.seconds;
    return f;
}

time_hms_t uptime_hms(void) { return time_to_hms(pit_seconds); }

/* ── calendar arithmetic (proleptic Gregorian) ──────────────────────── */

static const char *const MONTHS[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
static const char *const WEEKDAYS[7] = {"Monday",   "Tuesday", "Wednesday",
                                        "Thursday", "Friday",  "Saturday",
                                        "Sunday"};

const char *time_month_name(int m) {
    return m >= 1 && m <= 12 ? MONTHS[m - 1] : "?";
}
const char *time_weekday_name(int wd) {
    return wd >= 0 && wd <= 6 ? WEEKDAYS[wd] : "?";
}

bool time_is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int time_days_in_month(int m, int y) {
    static const uint8_t D[12] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12)
        return 0;
    return m == 2 && time_is_leap(y) ? 29 : D[m - 1];
}

bool time_date_valid(int y, int m, int d) {
    return y >= 1 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 &&
           d <= time_days_in_month(m, y);
}

/* Days since 1970-01-01 (negative before), H. Hinnant's algorithm: March
 * is month 0 of the shifted year, so February's leap day comes last. */
int32_t time_days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    int32_t yoe = y - era * 400;                                  /* 0..399 */
    int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* 0..365 */
    int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; /* 0..146096 */
    return era * 146097 + doe - 719468;
}

void time_civil_from_days(int32_t z, int *y, int *m, int *d) {
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    int32_t doe = z - era * 146097;
    int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int32_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* 0 = Monday .. 6 = Sunday (1970-01-01 was a Thursday) */
int time_weekday(int y, int m, int d) {
    int32_t z = time_days_from_civil(y, m, d);
    int w = (int)((z + 3) % 7);
    return w < 0 ? w + 7 : w;
}

int time_day_of_year(int y, int m, int d) {
    return (int)(time_days_from_civil(y, m, d) -
                 time_days_from_civil(y, 1, 1)) +
           1;
}

/* ISO 8601 week: weeks start on Monday, week 1 holds the first Thursday */
int time_iso_week(int y, int m, int d) {
    int32_t z = time_days_from_civil(y, m, d);
    int32_t thu = z - time_weekday(y, m, d) + 3; /* Thursday of this week */
    int ty, tm, td;
    time_civil_from_days(thu, &ty, &tm, &td);
    return (int)((thu - time_days_from_civil(ty, 1, 1)) / 7) + 1;
}

/* move a full date/time by `secs` seconds (any sign) */
static time_full_t shift(time_full_t t, int32_t secs) {
    int32_t z = time_days_from_civil((int)t.year, t.month, t.day);
    int32_t s = (int32_t)t.hours * 3600 + t.minutes * 60 + t.seconds + secs;
    z += s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
    s -= (s >= 0 ? s / 86400 : -((-s + 86399) / 86400)) * 86400;
    int y, m, d;
    time_civil_from_days(z, &y, &m, &d);
    t.year = (uint32_t)y;
    t.month = (uint8_t)m;
    t.day = (uint8_t)d;
    t.hours = (uint8_t)(s / 3600);
    t.minutes = (uint8_t)(s / 60 % 60);
    t.seconds = (uint8_t)(s % 60);
    return t;
}

/* ── the CMOS real-time clock ─────────────────────────────────────────
 * It keeps UTC here; the time zone of Settings is added for display.
 * Register B says how the BIOS set it up: bit 2 binary (else BCD), bit 1
 * 24-hour (else 12-hour, PM = bit 7 of the hour). Register 0x32 is the
 * century on most AT BIOSes. */

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static void cmos_write(uint8_t reg, uint8_t v) {
    outb(CMOS_ADDR, reg);
    outb(CMOS_DATA, v);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}
static uint8_t bin_to_bcd(uint8_t v) {
    return (uint8_t)((v / 10) << 4 | v % 10);
}

/* a century byte we trust: 19 or 20, BCD or binary */
static int rtc_century(bool binary) {
    uint8_t c = cmos_read(0x32);
    int v = binary ? c : bcd_to_bin(c);
    return v == 19 || v == 20 ? v : -1;
}

static bool rtc_updating(void) { return cmos_read(0x0A) & 0x80; }

static void rtc_raw(uint8_t r[6]) {
    static const uint8_t REG[6] = {0x00, 0x02, 0x04, 0x07, 0x08, 0x09};
    for (int i = 0; i < 6; i++)
        r[i] = cmos_read(REG[i]);
}

time_full_t time_rtc(void) {
    uint8_t a[6], b[6];
    uint32_t guard = 100000;
    while (rtc_updating() && --guard) {
    }
    rtc_raw(a);
    /* read until two reads agree: an update can land in between */
    for (int tries = 0; tries < 5; tries++) {
        guard = 100000;
        while (rtc_updating() && --guard) {
        }
        rtc_raw(b);
        bool same = true;
        for (int i = 0; i < 6; i++)
            same &= a[i] == b[i];
        if (same)
            break;
        for (int i = 0; i < 6; i++)
            a[i] = b[i];
    }

    uint8_t regb = cmos_read(0x0B);
    bool binary = regb & 0x04, h24 = regb & 0x02;
    bool pm = !h24 && (a[2] & 0x80);
    a[2] &= 0x7F;
    if (!binary)
        for (int i = 0; i < 6; i++)
            a[i] = bcd_to_bin(a[i]);
    if (!h24) /* 12 AM = 0 h, 12 PM = 12 h */
        a[2] = (uint8_t)(a[2] % 12 + (pm ? 12 : 0));

    time_full_t t;
    t.seconds = a[0];
    t.minutes = a[1];
    t.hours = a[2];
    t.day = a[3];
    t.month = a[4];
    int c = rtc_century(binary);
    if (c > 0)
        t.year = (uint32_t)(c * 100 + a[5]);
    else /* no century byte: 80-99 = 19xx */
        t.year = a[5] < 80 ? 2000u + a[5] : 1900u + a[5];
    return t;
}

bool time_rtc_set(const time_full_t *t) {
    if (!t || t->year < 1980 || t->year > 2099 ||
        !time_date_valid((int)t->year, t->month, t->day) || t->hours > 23 ||
        t->minutes > 59 || t->seconds > 59)
        return false;
    uint8_t regb = cmos_read(0x0B);
    bool binary = regb & 0x04, h24 = regb & 0x02;
    int century = rtc_century(binary);

    uint8_t v[7];
    v[0] = t->seconds;
    v[1] = t->minutes;
    uint8_t h = t->hours;
    bool pm = false;
    if (!h24) {
        pm = h >= 12;
        h = (uint8_t)(h % 12 ? h % 12 : 12);
    }
    v[2] = h;
    /* RTC weekday: 1 = Sunday .. 7 = Saturday */
    v[3] =
        (uint8_t)((time_weekday((int)t->year, t->month, t->day) + 1) % 7 + 1);
    v[4] = t->day;
    v[5] = t->month;
    v[6] = (uint8_t)(t->year % 100);
    if (!binary)
        for (int i = 0; i < 7; i++)
            v[i] = bin_to_bcd(v[i]);
    if (pm)
        v[2] |= 0x80;

    /* no interrupt handler touches the CMOS ports, so no cli is needed */
    cmos_write(0x0B, regb | 0x80); /* SET: stop updates while writing */
    static const uint8_t REG[7] = {0x00, 0x02, 0x04, 0x06, 0x07, 0x08, 0x09};
    for (int i = 0; i < 7; i++)
        cmos_write(REG[i], v[i]);
    if (century > 0) {
        uint8_t c = (uint8_t)(t->year / 100);
        cmos_write(0x32, binary ? c : bin_to_bcd(c));
    }
    cmos_write(0x0B, regb & 0x7F);
    return true;
}

time_full_t time_rtc_local(void) {
    return shift(time_rtc(), (int8_t)g_cfg.timezone_offset * 3600);
}

bool time_set_local(const time_full_t *local) {
    if (!local || !time_date_valid((int)local->year, local->month, local->day))
        return false;
    time_full_t utc = shift(*local, -(int8_t)g_cfg.timezone_offset * 3600);
    return time_rtc_set(&utc);
}

uint16_t time_from_rtc(void) {
    time_full_t t = time_rtc();
    return ((t.hours & 0x1F) << 11) | ((t.minutes & 0x3F) << 5) |
           ((t.seconds / 2) & 0x1F);
}

uint16_t date_from_rtc(void) {
    time_full_t t = time_rtc();
    uint16_t y = (t.year > 1980) ? (uint16_t)(t.year - 1980) : 0;
    return ((y & 0x7F) << 9) | ((t.month & 0x0F) << 5) | (t.day & 0x1F);
}
