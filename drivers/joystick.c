// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/joystick.h"
#include "interrupts/idt.h"
#include "lib/io.h"
#include "lib/mem.h"

joyvar joy;

/* One PIT count is 0.84 us, but mode 3 counts down by two: a "unit" below
 * is half of that, and the counter wraps every PIT_DIVISOR units. */
#define PIT_LATCH 0x00
#define TIMEOUT 4200 /* units: 1.8 ms, more than a 100 kOhm axis needs */
#define MIN_UNITS 30 /* a real axis takes at least 24 us */
#define MAX_LOOPS 6000
#define MIN_TRAVEL 100 /* least units between an axis' extremes */

static uint16_t s_raw[JOY_AXES];
static uint16_t s_auto[JOY_AXES]; /* the centre until calibrated */
static bool s_tracking;
static int s_miss;

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f)::"memory");
    return f;
}

static inline void irq_restore(uint32_t f) {
    __asm__ volatile("pushl %0; popfl" ::"r"(f) : "memory", "cc");
}

static uint16_t pit_count(void) {
    outb(PIT_CMD, PIT_LATCH);
    uint8_t lo = inb(PIT_CHANNEL0);
    uint8_t hi = inb(PIT_CHANNEL0);
    return (uint16_t)(lo | hi << 8);
}

/* Starts the timers and times each axis; `want` = axes to wait for (0 =
 * wait out the timeout). Returns the axes that answered. */
static uint8_t port_read(uint16_t t[JOY_AXES], uint8_t *buttons, uint8_t want) {
    uint8_t done = 0;
    for (int i = 0; i < JOY_AXES; i++)
        t[i] = JOY_RAW_NONE;
    uint32_t fl = irq_save();
    outb(JOY_PORT, 0xFF);
    uint16_t prev = pit_count();
    uint32_t el = 0;
    uint8_t v = inb(JOY_PORT);
    *buttons = (uint8_t)(~v >> 4 & 0x0F);
    for (int n = 0; n < MAX_LOOPS && el < TIMEOUT; n++) {
        v = inb(JOY_PORT);
        uint16_t now = pit_count();
        el += prev >= now ? (uint32_t)(prev - now)
                          : (uint32_t)(prev + PIT_DIVISOR - now);
        prev = now;
        for (int i = 0; i < JOY_AXES; i++)
            if (!(v & (1 << i)) && !(done & (1 << i))) {
                done |= (uint8_t)(1 << i);
                if (el >= MIN_UNITS && el < TIMEOUT)
                    t[i] = (uint16_t)el;
            }
        if (want && (done & want) == want)
            break;
    }
    irq_restore(fl);
    uint8_t ok = 0;
    for (int i = 0; i < JOY_AXES; i++)
        if (t[i] != JOY_RAW_NONE)
            ok |= (uint8_t)(1 << i);
    return ok;
}

uint16_t joy_raw(int axis) {
    return axis >= 0 && axis < JOY_AXES ? s_raw[axis] : JOY_RAW_NONE;
}

bool joy_detect(void) {
    uint16_t t[JOY_AXES];
    uint8_t b;
    uint8_t a = port_read(t, &b, 0);
    /* twice, so noise on an open port doesn't count */
    uint8_t a2 = a ? port_read(s_raw, &b, 0) : 0;
    joy.axes = (uint8_t)(a & a2);
    joy.present = joy.axes != 0;
    joy.buttons = joy.pressed = 0;
    for (int i = 0; i < JOY_AXES; i++) {
        s_auto[i] = joy.axes & (1 << i) ? s_raw[i] : 0;
        joy.axis[i] = 0;
        if (!(joy.axes & (1 << i)))
            s_raw[i] = JOY_RAW_NONE;
    }
    s_miss = 0;
    return joy.present;
}

void joy_init(void) { joy_detect(); }

/* raw -> -100..100 */
static int scale(int i, uint16_t raw) {
    const joy_cfg_t *c = &g_cfg.joy;
    int mid = c->mid[i], lo = c->min[i], hi = c->max[i];
    if (!c->calibrated || !(lo < mid && mid < hi)) { /* a guess: centred now */
        mid = s_auto[i];
        int half = mid * 3 / 4;
        lo = mid - half;
        hi = mid + half;
    }
    int v = (int)raw - mid;
    int span = v >= 0 ? hi - mid : mid - lo;
    if (span <= 0)
        return 0;
    int pct = v * 100 / span;
    if (pct > 100)
        pct = 100;
    if (pct < -100)
        pct = -100;
    int dz = c->deadzone > 60 ? 60 : c->deadzone;
    int mag = pct < 0 ? -pct : pct;
    mag = mag <= dz ? 0 : (mag - dz) * 100 / (100 - dz);
    pct = pct < 0 ? -mag : mag;
    return c->invert & (1 << i) ? -pct : pct;
}

void joy_update(void) {
    if (!joy.present) {
        joy.pressed = 0;
        return;
    }
    uint16_t t[JOY_AXES];
    uint8_t b;
    uint8_t got = port_read(t, &b, joy.axes);
    if (!got) {
        if (++s_miss >= 30) { /* unplugged */
            joy.present = false;
            joy.axes = 0;
            joy.buttons = joy.pressed = 0;
            for (int i = 0; i < JOY_AXES; i++)
                joy.axis[i] = 0;
        }
        return;
    }
    s_miss = 0;
    for (int i = 0; i < JOY_AXES; i++) {
        if (!(joy.axes & (1 << i)))
            continue;
        if (t[i] != JOY_RAW_NONE) /* a missed one keeps its last value */
            s_raw[i] = t[i];
        if (s_tracking && s_raw[i] != JOY_RAW_NONE) {
            joy_cfg_t *c = &g_cfg.joy;
            if (s_raw[i] < c->min[i])
                c->min[i] = s_raw[i];
            if (s_raw[i] > c->max[i])
                c->max[i] = s_raw[i];
        }
        joy.axis[i] = scale(i, s_raw[i]);
    }
    joy.pressed = (uint8_t)(b & ~joy.buttons);
    joy.buttons = b;
}

bool joy_cal_center(void) {
    if (!joy.present)
        return false;
    uint32_t sum[JOY_AXES] = {0}, n[JOY_AXES] = {0};
    for (int k = 0; k < 8; k++) {
        uint16_t t[JOY_AXES];
        uint8_t b;
        port_read(t, &b, joy.axes);
        for (int i = 0; i < JOY_AXES; i++)
            if (t[i] != JOY_RAW_NONE) {
                sum[i] += t[i];
                n[i]++;
            }
    }
    joy_cfg_t *c = &g_cfg.joy;
    bool any = false;
    for (int i = 0; i < JOY_AXES; i++) {
        if (!n[i])
            continue;
        c->mid[i] = c->min[i] = c->max[i] = (uint16_t)(sum[i] / n[i]);
        s_auto[i] = c->mid[i];
        any = true;
    }
    c->calibrated = 0; /* until joy_cal_done */
    return any;
}

void joy_cal_range(bool on) { s_tracking = on; }

bool joy_cal_done(void) {
    s_tracking = false;
    joy_cfg_t *c = &g_cfg.joy;
    for (int i = 0; i < JOY_AXES; i++) {
        if (!(joy.axes & (1 << i)))
            continue;
        if (c->max[i] - c->mid[i] < MIN_TRAVEL / 2 ||
            c->mid[i] - c->min[i] < MIN_TRAVEL / 2)
            return false;
    }
    c->calibrated = 1;
    return true;
}

void joy_cal_reset(void) {
    s_tracking = false;
    joy_cfg_t *c = &g_cfg.joy;
    memset(c, 0, sizeof(*c));
    c->deadzone = 10;
}
