#include "os/os.h"
#include "lib/primitive_graphics.h"
#include "lib/mem.h"
#include "ui/ui.h"
#include "io/ps2.h"
#include "config/config.h"

#define MAX_APPS 8

static app_descriptor *apps[MAX_APPS];
static int app_count = 0;

menubar g_menubar;
static bool gui_should_exit = false;

void os_register_app(app_descriptor *app) {
    if (app_count >= MAX_APPS) return;
    apps[app_count++] = app;
}

void os_run(void) {
    gui_should_exit = false;

    for (int i = 0; i < app_count; i++)
        if (apps[i]->init) apps[i]->init();

    while (!gui_should_exit) {
        ps2_update();

        for (int i = 0; i < app_count; i++)
            if (apps[i]->on_frame) apps[i]->on_frame();

        clear_screen(0xD0);

        wm_sync_menubar(&g_menubar);
        menubar_layout(&g_menubar);
        menubar_update(&g_menubar);
        wm_update_all();

        wm_draw_all();
        menubar_draw(&g_menubar);

        draw_cursor(mouse.x, mouse.y);
        blit();
    }
}

void os_request_exit(void) {
    gui_should_exit = true;
}
