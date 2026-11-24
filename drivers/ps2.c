// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/ps2.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/sermouse.h"
#include "lib/core.h"

/*
 * Frame-based 8042 reads can lag behind mouse packets. In the desktop,
 * ps2_irq_poll reads each timer tick, moves the pointer, and queues keys;
 * the text shell continues reading through ps2_update().
 */

#define KRING 64
static volatile uint8_t s_kring[KRING];
static volatile uint8_t s_khead, s_ktail;
static volatile bool s_irq_mode;

void ps2_init(void) {
    mouse_init();
    kb_init();
}

void ps2_irq_mode(bool on) {
    if (on == s_irq_mode)
        return;
    __asm__ volatile("cli");
    s_khead = s_ktail = 0;
    s_irq_mode = on;
    __asm__ volatile("sti");
}

bool ps2_irq_active(void) { return s_irq_mode; }

/* timer interrupt (interrupts off) */
void ps2_irq_poll(void) {
    if (!s_irq_mode)
        return;
    for (int n = 0; n < 16; n++) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & 0x01))
            break;
        uint8_t b = inb(PS2_DATA);
        if (st & 0x20) {
            mouse_process_byte(b);
        } else {
            uint8_t next = (uint8_t)((s_khead + 1) % KRING);
            if (next != s_ktail) { /* full: drop */
                s_kring[s_khead] = b;
                s_khead = next;
            }
        }
    }
    sermouse_poll();
}

int ps2_key_byte(void) {
    if (s_ktail == s_khead)
        return -1;
    uint8_t b = s_kring[s_ktail];
    s_ktail = (uint8_t)((s_ktail + 1) % KRING);
    return b;
}

void ps2_update(void) {
    kb_update();
    if (s_irq_mode) {
        /* one key per frame: several typed during a slow frame are not
         * lost, they come in the next frames */
        int b;
        while (!kb.key_pressed && (b = ps2_key_byte()) >= 0)
            kb_process_byte((uint8_t)b);
    } else {
        sermouse_poll();
        while (1) {
            unsigned char status = inb(PS2_STATUS);
            if (!(status & 0x01))
                break;
            unsigned char byte = inb(PS2_DATA);
            if (status & 0x20)
                mouse_process_byte(byte);
            else
                kb_process_byte(byte);
        }
    }
    mouse_update(); /* what moved since the last frame */
}
