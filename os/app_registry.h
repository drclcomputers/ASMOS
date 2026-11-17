#ifndef APP_REGISTRY_H
#define APP_REGISTRY_H

#include "os/os.h"

/* Apps menu groups (submenus); APP_TOP = directly in Apps */
enum {
    APP_TOP,
    APP_ACCESSORIES,
    APP_GAMES,
    APP_MULTIMEDIA,
    APP_SYSTEM,
    APP_GROUPS
};
extern const char *const APP_GROUP_NAMES[APP_GROUPS];

typedef struct {
    app_descriptor *desc;
    const char *menu_label;
    int group; /* APP_* */
    /* only in the Apps menu when started from the setup floppy (Setup):
     * no shortcut on an installed disk; "app run Setup" still starts it */
    bool setup_only;
} registered_app_t;

extern registered_app_t app_registry[];
extern int app_registry_count;

#endif
