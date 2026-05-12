#include "config/runtime_config.h"
#include "config/config.h"
#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"

extern int g_video_mode;

os_config_t g_cfg;

void cfg_init_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.magic = CFG_MAGIC;
    g_cfg.version = CFG_VERSION;
    g_cfg.wallpaper_pattern = WALLPAPER_DOTS;
    g_cfg.timezone_offset = 0;
    g_cfg.start_in_gui = 1;
    g_cfg.play_bootchime = 1;
    g_cfg.sound_enabled = 1;
    g_cfg.wallpaper_main_color = LIGHT_BLUE;
    g_cfg.wallpaper_secondary_color = LIGHT_CYAN;
    g_cfg.filef_single_window = 0;
    g_cfg.reduce_motion = 0;
}

bool cfg_load(void) {
    fat_file_t f;
    if (!fs_open(CFG_PATH, &f))
        return false;

    os_config_t tmp;
    int n = fs_read(&f, &tmp, sizeof(os_config_t));
    fs_close(&f);

    if (n < (int)sizeof(os_config_t))
        return false;
    if (tmp.magic != CFG_MAGIC)
        return false;

    g_cfg = tmp;

    if (g_cfg.version < 2)
        g_cfg.filef_single_window = 0;
    return true;
}

bool cfg_save(void) {
    dir_entry_t de;
    if (fs_find(CFG_PATH, &de))
        fs_delete(CFG_PATH);

    fat_file_t f;
    if (!fs_create(CFG_PATH, &f))
        return false;

    g_cfg.magic = CFG_MAGIC;
    g_cfg.version = CFG_VERSION;

    int written = fs_write(&f, &g_cfg, sizeof(os_config_t));
    fs_close(&f);
    
    *(volatile uint8_t *)0x0602 = (uint8_t)g_video_mode;
    
    return written == sizeof(os_config_t);
}
