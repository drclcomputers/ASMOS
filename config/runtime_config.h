#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include "lib/types.h"

// Magic number to validate the config file
#define CFG_MAGIC       0x4153434F   // "ASCO"
#define CFG_VERSION     1
#define CFG_PATH        "/ASMOS.CFG"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;

    uint8_t  wallpaper_pattern;

    uint8_t  timezone_offset;
    uint8_t  start_in_gui;
    uint8_t  play_bootchime;
    uint8_t  sound_enabled;

    uint8_t  wallpaper_main_color;
    uint8_t  wallpaper_secondary_color;

    uint8_t  _reserved[54];
} os_config_t;

extern os_config_t g_cfg;

void cfg_init_defaults(void);
bool cfg_load(void);
bool cfg_save(void);

#endif
