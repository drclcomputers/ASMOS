#ifndef JOYSTICK_H
#define JOYSTICK_H

#include "config/runtime_config.h"
#include "lib/core.h"

/*
 * Game port (0x201): up to 4 axes and 4 buttons, two sticks A and B. An
 * axis is a resistor, so its value is a time: writing to the port starts one
 * timer per axis and the axis bit stays high until it runs out. The time is
 * measured with the PIT (CPU speed doesn't matter), interrupts off for
 * about a millisecond per read.
 *
 * Probed once at start (joy_init) and again on joy_detect(): there is no
 * plug-in signal. joy_update() reads the stick once a frame; apps use `joy`.
 */

#define JOY_PORT 0x201
#define JOY_AXES 4 /* X, Y of stick A, X, Y of stick B */
#define JOY_BUTTONS 4
#define JOY_RAW_NONE 0xFFFF /* an axis that did not answer */

typedef struct {
    bool present;       /* a stick answered the last probe */
    uint8_t axes;       /* bit per axis that answers (0x03: a plain stick) */
    int axis[JOY_AXES]; /* -100 .. 100, 0 centred, after calibration */
    uint8_t buttons;    /* bit n: button n+1 held */
    uint8_t pressed;    /* bits that went down since the last frame */
} joyvar;

extern joyvar joy;

void joy_init(void);
bool joy_detect(void); /* probe again; true if a stick answered */
void joy_update(void); /* once a frame: refresh `joy` */

/* what the last read measured (driver units, JOY_RAW_NONE if none) */
uint16_t joy_raw(int axis);

/* Calibration, kept in g_cfg.joy (Settings > Devices > Joystick):
 * 1. joy_cal_center() with the stick released,
 * 2. joy_cal_range(true), roll the stick to every extreme,
 * 3. joy_cal_range(false) and joy_cal_done() */
bool joy_cal_center(void);
void joy_cal_range(bool on);
bool joy_cal_done(void); /* false if an axis did not move enough */
void joy_cal_reset(void);

#endif
