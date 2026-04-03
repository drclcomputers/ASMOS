#ifndef OS_H
#define OS_H

#include "lib/types.h"
#include "lib/alloc.h"
#include "ui/menubar.h"

typedef void  (*app_init_fn)(void *state);
typedef void  (*app_frame_fn)(void *state);
typedef void  (*app_destroy_fn)(void *state);

typedef struct {
    const char     *name;
    uint32_t        state_size;   // bytes to kmalloc for per-instance state
    app_init_fn     init;         // called once after state is allocated
    app_frame_fn    on_frame;     // called every frame while running
    app_destroy_fn  destroy;      // called before state is freed; must
                                  // wm_unregister all windows it opened
} app_descriptor;


typedef struct {
    app_descriptor *desc;
    void           *state;
    bool            running;
} app_instance_t;

void os_install_app(app_descriptor *desc);

app_instance_t *os_launch_app(app_descriptor *desc);

void os_quit_app(app_instance_t *inst);

void os_quit_app_by_desc(app_descriptor *desc);

app_instance_t *os_find_instance(app_descriptor *desc);

app_descriptor *os_find_app(const char *name);

void os_run(void);

void os_request_exit(void);

#define MAX_INSTALLED_APPS  32
#define MAX_RUNNING_APPS    16

extern app_descriptor *installed_apps[MAX_INSTALLED_APPS];
extern int             installed_app_count;

extern app_instance_t  running_apps[MAX_RUNNING_APPS];
extern int             running_app_count;

extern menubar g_menubar;

#endif
