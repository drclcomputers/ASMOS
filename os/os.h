#ifndef OS_H
#define OS_H

#include "lib/types.h"
#include "ui/menubar.h"

typedef void (*app_init_fn)(void);
typedef void (*app_event_fn)(void);

typedef struct {
    char        *name;
    app_init_fn  init;
    app_event_fn on_frame;
} app_descriptor;

void os_register_app(app_descriptor *app);

void os_run(void);

#endif
