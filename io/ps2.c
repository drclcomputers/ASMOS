#include "io/ps2.h"
#include "io/keyboard.h"
#include "io/mouse.h"
#include "lib/core.h"

void ps2_init(void) {
	mouse_init();
	kb_init();
}

void ps2_update(void) {
	kb_update();
	mouse_update();
    while (1) {
        unsigned char status = inb(PS2_STATUS);
        if (!(status & 0x01)) break;

        unsigned char byte = inb(PS2_DATA);

        if (status & 0x20)
            mouse_process_byte(byte);
        else
            kb_process_byte(byte);
    }
}
