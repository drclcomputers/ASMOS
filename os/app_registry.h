#ifndef APP_REGISTRY_H
#define APP_REGISTRY_H

#include "os/os.h"

typedef struct {
    app_descriptor *desc;
    const char     *menu_label;
} registered_app_t;

extern registered_app_t app_registry[];
extern int              app_registry_count;

#endif
