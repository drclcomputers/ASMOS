#include "drivers/mouse.h"
#include "config/config.h"
#include "drivers/sermouse.h"
#include "lib/core.h"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64

mousevar mouse = {160, 100, 0, 0, 0, 0, 0, 0, 0};
static int s_kinds;

static uint8_t packet[3];
static int packet_idx = 0;

static void ps2_wait_write(void) {
    unsigned int t = 100000;
    while ((inb(PS2_STATUS) & 0x02) && --t)
        ;
}

static void ps2_wait_read(void) {
    unsigned int t = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && --t)
        ;
}

static void mouse_write(unsigned char val) {
    ps2_wait_write();
    outb(PS2_CMD, 0xD4);
    ps2_wait_write();
    outb(PS2_DATA, val);
}

/* Returns -1 on timeout (no aux device / no PS/2 port). */
static int mouse_read(void) {
    unsigned int t = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && --t)
        ;
    return t ? inb(PS2_DATA) : -1;
}

void mouse_init(void) {
    ps2_wait_write();
    outb(PS2_CMD, 0xA8);

    ps2_wait_write();
    outb(PS2_CMD, 0x20);
    ps2_wait_read();
    unsigned char cmd = inb(PS2_DATA);

    cmd |= 0x02;
    cmd &= ~0x20;

    ps2_wait_write();
    outb(PS2_CMD, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, cmd);

    mouse_write(0xF6);
    mouse_read();

    mouse_write(0xF4);
    if (mouse_read() == 0xFA) /* ACK: a PS/2 mouse is there */
        s_kinds |= MOUSE_PS2;

    if (sermouse_init())
        s_kinds |= MOUSE_SERIAL;
}

mouse_kind_t mouse_kinds(void) { return (mouse_kind_t)s_kinds; }

void mouse_apply(int dx, int dy, bool left, bool right, bool middle) {
    bool prev_left = mouse.left, prev_right = mouse.right;
    mouse.left = left;
    mouse.right = right;
    mouse.middle = middle;
    mouse.left_clicked |= !prev_left && left;
    mouse.right_clicked |= !prev_right && right;
    mouse.dx += dx;
    mouse.dy += dy;
    mouse.x += dx;
    mouse.y += dy;
    if (mouse.x < 0)
        mouse.x = 0;
    if (mouse.x >= SCREEN_WIDTH)
        mouse.x = SCREEN_WIDTH - 1;
    if (mouse.y < 0)
        mouse.y = 0;
    if (mouse.y >= SCREEN_HEIGHT)
        mouse.y = SCREEN_HEIGHT - 1;
}

void mouse_update(void) {
    mouse.dx = 0;
    mouse.dy = 0;
    mouse.left_clicked = false;
    mouse.right_clicked = false;
}

void mouse_process_byte(uint8_t b) {
    if (packet_idx == 0 && !(b & 0x08))
        return;

    packet[packet_idx++] = b;

    if (packet_idx < 3)
        return;
    packet_idx = 0;

    uint8_t b1 = packet[0];
    uint8_t b2 = packet[1];
    uint8_t b3 = packet[2];

    int dx = (int)b2 - ((b1 & 0x10) ? 256 : 0);
    int dy = -((int)b3 - ((b1 & 0x20) ? 256 : 0));
    mouse_apply(dx, dy, (b1 & 0x01) != 0, (b1 & 0x02) != 0, (b1 & 0x04) != 0);
}
