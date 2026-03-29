#include "io/mouse.h"
#include "lib/io.h"
#include "config/config.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

mousevar mouse = { 160, 100, 0, 0, 0, 0, 0, 0, 0 };

static uint8_t  packet[3];
static int      packet_idx = 0;

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
    mouse.dx          = 0;
    mouse.dy          = 0;
    mouse.left_clicked  = false;
    mouse.right_clicked = false;
}

void mouse_process_byte(uint8_t b) {
    if (packet_idx == 0 && !(b & 0x08)) return;

    packet[packet_idx++] = b;

    if (packet_idx < 3) return;
    packet_idx = 0;

    uint8_t b1 = packet[0];
    uint8_t b2 = packet[1];
    uint8_t b3 = packet[2];

    int dx =  (int)b2 - ((b1 & 0x10) ? 256 : 0);
    int dy = -((int)b3 - ((b1 & 0x20) ? 256 : 0));

    mouse.dx = dx;
    mouse.dy = dy;

    bool prev_left  = mouse.left;
    bool prev_right = mouse.right;

    mouse.left   = (b1 & 0x01) != 0;
    mouse.right  = (b1 & 0x02) != 0;
    mouse.middle = (b1 & 0x04) != 0;

    mouse.left_clicked  = (!prev_left  && mouse.left);
    mouse.right_clicked = (!prev_right && mouse.right);

    mouse.x += dx;
    mouse.y += dy;

    if (mouse.x < 0)              mouse.x = 0;
    if (mouse.x >= SCREEN_WIDTH)  mouse.x = SCREEN_WIDTH  - 1;
    if (mouse.y < 0)              mouse.y = 0;
    if (mouse.y >= SCREEN_HEIGHT) mouse.y = SCREEN_HEIGHT - 1;
}
