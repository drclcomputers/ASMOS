#ifndef DESKTOP_FS_H
#define DESKTOP_FS_H

#include "lib/core.h"
#include "fs/fat16.h"

#define DESKTOP_PATH        "/DESKTOP"
#define DESKTOP_CLUSTER_NONE 0xFFFF

#define DESKTOP_ITEM_FILE   0
#define DESKTOP_ITEM_DIR    1
#define DESKTOP_ITEM_APP    2

#define DESKTOP_MAX_ITEMS   32
#define DESKTOP_NAME_MAX    13
#define DESKTOP_APP_EXT     "APP"

typedef struct {
    char    name[DESKTOP_NAME_MAX];
    uint8_t kind;
    bool    used;

    int     x, y;
    bool    selected;
    bool    dragging;
    int     drag_off_x, drag_off_y;
} desktop_item_t;

void desktop_fs_init(void);

void desktop_fs_reload(void);

void desktop_fs_set_dirty(void);

bool desktop_fs_is_dirty(void);

desktop_item_t *desktop_fs_items(void);
int desktop_fs_count(void);
bool desktop_fs_add_app(const char *app_name);
bool desktop_fs_delete(int idx);
bool desktop_fs_rename(int idx, const char *new_name);
void desktop_fs_move_icon(int idx, int new_x, int new_y);

uint16_t desktop_fs_cluster(void);
const char *desktop_fs_path(void);

#endif
