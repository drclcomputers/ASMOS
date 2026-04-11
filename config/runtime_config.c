#include "config/runtime_config.h"
#include "config/config.h"
#include "fs/fat16.h"
#include "lib/mem.h"
#include "lib/string.h"

os_config_t g_cfg;

void cfg_init_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.magic                  = CFG_MAGIC;
    g_cfg.version                = CFG_VERSION;
    g_cfg.wallpaper_pattern      = 1;
    g_cfg.timezone_offset        = 0;
    g_cfg.start_in_gui           = 1;
    g_cfg.play_bootchime         = 1;
    g_cfg.sound_enabled          = 1;
    g_cfg.wallpaper_main_color   = BLUE;
    g_cfg.wallpaper_secondary_color = LIGHT_BLUE;
}

bool cfg_load(void) {
    fat16_file_t f;
    if (!fat16_open(CFG_PATH, &f)) return false;

    os_config_t tmp;
    int n = fat16_read(&f, &tmp, sizeof(os_config_t));
    fat16_close(&f);

    if (n < (int)sizeof(os_config_t)) return false;
    if (tmp.magic != CFG_MAGIC)       return false;

    g_cfg = tmp;
    return true;
}

bool cfg_save(void) {
    dir_entry_t de;
    if (fat16_find(CFG_PATH, &de)) fat16_delete(CFG_PATH);

    fat16_file_t f;
    if (!fat16_create(CFG_PATH, &f)) return false;

    g_cfg.magic   = CFG_MAGIC;
    g_cfg.version = CFG_VERSION;

    int written = fat16_write(&f, &g_cfg, sizeof(os_config_t));
    fat16_close(&f);
    return written == sizeof(os_config_t);
}
