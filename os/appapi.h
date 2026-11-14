#ifndef OS_APPAPI_H
#define OS_APPAPI_H

/* Stable function table handed to v2 .APP packages as arg 0 of
 * init/on_frame/destroy.  Apps must NOT reference kernel symbols directly
 * (the flat binary has no linker access to them) – go through this table.
 * Only ever append new members; bump `size`, not `version`. */

#include "io/mouse.h"
#include "lib/core.h"
#include "ui/window.h"

#define OS_API_VERSION 1

typedef struct os_api {
    uint32_t version;
    uint32_t size; /* sizeof(os_api_t) – check before using newer members */

    void *(*kmalloc)(uint32_t n);
    void (*kfree)(void *p);
    uint32_t (*time_millis)(void);

    window *(*wm_register)(const window_spec_t *spec);
    void (*wm_unregister)(window *w);
    void (*wm_focus)(window *w);
    void (*window_add_widget)(window *w, widget wg);

    void (*draw_dot)(int x, int y, unsigned char c);
    void (*draw_line)(int x0, int y0, int x1, int y1, unsigned char c);
    void (*draw_rect)(int x, int y, int w, int h, unsigned char c);
    void (*fill_rect)(int x, int y, int w, int h, unsigned char c);
    void (*draw_char)(int x, int y, char ch, unsigned char c, int size);
    void (*draw_string)(int x, int y, char *s, unsigned char c, int size);

    const mousevar *mouse;

    /* Ask the OS to close this app instance (pass your own state ptr). */
    void (*quit_self)(void *state);
} os_api_t;

#endif
