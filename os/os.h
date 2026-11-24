// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef OS_H
#define OS_H

#include "lib/core.h"
#include "lib/memory.h"
#include "ui/menubar.h"
#include "ui/window.h"

#define MAX_INSTALLED_APPS 256
#define MAX_RUNNING_APPS 48

typedef void (*app_init_fn)(void *state);
typedef void (*app_frame_fn)(void *state);
typedef void (*app_destroy_fn)(void *state);

typedef struct app_descriptor_s {
    const char *name;
    uint32_t state_size;
    app_init_fn init;
    app_frame_fn on_frame;
    app_destroy_fn destroy;
    bool single_instance;
    bool opaque_state; /* true: state does NOT start with a window* */
    /* optional: where the state lives (dynamic apps: their own heap) */
    void *(*state_alloc)(struct app_descriptor_s *d, uint32_t size);
    void (*state_free)(struct app_descriptor_s *d, void *state);
} app_descriptor;

typedef struct {
    app_descriptor *desc;
    void *state;
    bool running;
    bool wants_quit;
    int task_slot;
    bool suspended; /* stopped while not responding: its code is not called
                       again (Monitor: Switch To resumes, End Task ends) */
} app_instance_t;

void os_install_app(app_descriptor *desc);
void os_uninstall_app(app_descriptor *desc);
window *os_instance_window(const app_instance_t *a);
app_instance_t *os_launch_app(app_descriptor *desc);
void os_quit_app(app_instance_t *inst);
void os_quit_app_by_desc(app_descriptor *desc);
app_instance_t *os_find_instance(app_descriptor *desc);
int os_find_instances(app_descriptor *desc, app_instance_t **out, int max);
app_descriptor *os_find_app(const char *name);
void os_request_exit(void);
bool window_is_focused(window *win);
bool os_close_own_instance(window *win);

void os_tick_apps(void);

/* Built-in app callbacks run guarded: crashes or watchdog termination end
 * the app, not the OS. culprit identifies the instance; NULL reports only. */
app_instance_t *os_instance_of_window(window *w);
void os_guard_call(app_instance_t *culprit, void (*fn)(void *, void *), void *a,
                   void *b, const char *what);
/* An instance stopped/ended by the watchdog: for Monitor to select. */
extern int g_os_stopped_app; /* index into running_apps, -1 none */
void os_reap_dead_apps(void);

extern app_descriptor *installed_apps[MAX_INSTALLED_APPS];
extern int installed_app_count;

extern app_instance_t running_apps[MAX_RUNNING_APPS];
extern int running_app_count;

extern menubar g_menubar;
extern bool g_menubar_click_consumed;
/* booted from the ASMOS setup floppy (kernel.c): Setup runs, nothing is
 * written to the hard disk until it says so (no ASMOS.CFG, no /DESKTOP) */
extern bool g_setup_mode;
extern bool gui_should_exit;

typedef struct {
    bool set;
    uint8_t hour;
    uint8_t minute;
    bool fired;
} global_alarm_t;

extern global_alarm_t g_alarm;

#endif
