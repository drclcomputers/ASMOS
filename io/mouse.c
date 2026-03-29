#include "io/mouse.h"
#include "lib/io.h"
#include "lib/utils.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

mousevar mouse = { 160, 100, 0, 0, 0, 0, 0, 0, 0 };

static void ps2_wait_write(void) {
    unsigned int t = 100000;
    while ((inb(PS2_STATUS) & 0x02) && --t);
}

static void ps2_wait_read(void) {
    unsigned int t = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && --t);
}

static void mouse_write(unsigned char val) {
    ps2_wait_write();
    outb(PS2_CMD, 0xD4);
    ps2_wait_write();
    outb(PS2_DATA, val);
}

static unsigned char mouse_read(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

void mouse_init(void) {
    ps2_wait_write();
    outb(PS2_CMD, 0xA8);

    ps2_wait_write();
    outb(PS2_CMD, 0x20);
    ps2_wait_read();
    unsigned char cmd = inb(PS2_DATA);

    cmd |=  0x02;
    cmd &= ~0x20;

    ps2_wait_write();
    outb(PS2_CMD, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, cmd);

    mouse_write(0xF6);
    mouse_read();

    mouse_write(0xF4);
    mouse_read();
}

void mouse_update(void) {
    if (!(inb(PS2_STATUS) & 0x01))
        return;

    unsigned char b1 = inb(PS2_DATA);

    if (!(b1 & 0x08))
        return;

    ps2_wait_read();
    unsigned char b2 = inb(PS2_DATA);

    ps2_wait_read();
    unsigned char b3 = inb(PS2_DATA);

    // The sign bits live in byte 1: bit 4 = X sign, bit 5 = Y sign.
    int dx = (int)b2 - ((b1 & 0x10) ? 256 : 0);
    int dy = (int)b3 - ((b1 & 0x20) ? 256 : 0);

    mouse.dx =  dx;
    mouse.dy = -dy;

    bool prev_left  = mouse.left;
    bool prev_right = mouse.right;

    mouse.left   = (b1 & 0x01) != 0;
    mouse.right  = (b1 & 0x02) != 0;
    mouse.middle = (b1 & 0x04) != 0;

    mouse.left_clicked  = (!prev_left  && mouse.left);
    mouse.right_clicked = (!prev_right && mouse.right);

    mouse.x += dx;
    mouse.y -= dy;

    if (mouse.x < 0)         mouse.x = 0;
    if (mouse.x >= SCREEN_WIDTH) mouse.x = SCREEN_WIDTH - 1;
    if (mouse.y < 0)         mouse.y = 0;
    if (mouse.y >= SCREEN_HEIGHT) mouse.y = SCREEN_HEIGHT - 1;
}
