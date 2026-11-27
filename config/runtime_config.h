// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include "lib/core.h"

#define CFG_MAGIC 0x4153434F
#define CFG_VERSION 4
#define CFG_PATH "/ASMOS.CFG"

typedef struct __attribute__((packed)) {
    uint16_t min[4], mid[4], max[4];
    uint8_t deadzone; /* percent of the travel that counts as centred */
    uint8_t invert;   /* bit per axis */
    uint8_t calibrated;
} joy_cfg_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;

    uint8_t wallpaper_pattern;
    uint8_t timezone_offset;
    uint8_t start_in_gui;
    uint8_t play_bootchime;
    uint8_t sound_enabled;
    uint8_t sb16_volume;
    uint8_t wallpaper_main_color;
    uint8_t wallpaper_secondary_color;
    uint8_t filef_single_window;
    uint8_t reduce_motion;
    uint8_t networking_enabled;
    uint8_t ip[4];
    uint8_t gateway[4];
    uint8_t netmask[4];
    uint8_t dns_server[4];
    uint8_t no_paging;     /* 1 = run without memory protection */
    uint8_t fpu_emulate;   /* 1 = use the x87 emulator even with an FPU */
    uint8_t classic_fonts; /* 1 = the old 4x6 font everywhere */
    uint8_t saver_minutes; /* screen saver after this idle time; 0 = never */
    uint8_t saver_effect;  /* ui/saver.h */
    /* Performance (Settings): 0 everywhere = everything on */
    uint8_t outline_drag;   /* move/resize show a frame, not the window */
    uint8_t clock_no_secs;  /* menu bar clock without seconds */
    uint8_t caret_no_blink; /* text cursor stays on */
    uint8_t no_tooltips;
    uint8_t
        no_power_anim; /* unused (the animation is gone); kept for the file */
    uint8_t fps_cap;   /* 0 = 60, 1 = 30, 2 = 20 frames a second */
    /* Printing (os/print.h) */
    uint8_t printer_type;  /* PRN_TEXT, PRN_ESCP, PRN_PCL */
    uint8_t printer_port;  /* 0 = LPT1 */
    uint8_t printer_no_ff; /* 1 = no form feed after a job */
    uint8_t ui_zoom2;      /* 1 = everything 2x (640x400 and up; restart) */
    uint8_t codepage;      /* characters 128-255: CP_WESTERN/CENTRAL/DOS */
    uint8_t net_dhcp;      /* 1 = the address from DHCP (network/dhcp.h) */
    uint8_t _reserved[19];
    uint8_t wall_custom[32];
    joy_cfg_t joy;
} os_config_t;

extern os_config_t g_cfg;

/* colour of pixel (x, y) of the custom pattern, 0..15 (it repeats) */
static inline uint8_t cfg_wall_px(int x, int y) {
    uint8_t b = g_cfg.wall_custom[(y & 7) * 4 + ((x & 7) >> 1)];
    return (uint8_t)(x & 1 ? b & 15 : b >> 4);
}
void cfg_default_pattern(uint8_t out[32]);

void cfg_init_defaults(void);
void cfg_boot(bool setup_mode);
/* the Performance settings: true = everything light (for a 386), false =
 * everything on */
void cfg_set_low_end(bool low);
bool cfg_is_low_end(void);
int cfg_frame_ms(void);
bool cfg_load(void);
bool cfg_save(void);

#endif
