#include "lib/utils.h"
#include "ui/ui.h"
#include "io/ps2.h"

void kmain(void) {
    ps2_init();

    while (1) {
        ps2_update();

        clear_screen(0x0F);

        draw_window(20, 20, 280, 140, "My Window");

        draw_cursor(mouse.x, mouse.y);

        if (mouse.left)
            fill_rect(1, 192, 60, 7, 0x00);

        blit();
    }
}
