#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "lib/types.h"

#define ESC        0x01
#define BACKSPACE  0x0E
#define TAB        0x0F
#define ENTER      0x1C
#define LCTRL      0x1D
#define LSHIFT     0x2A
#define RSHIFT     0x36
#define LALT       0x38
#define CAPSLOCK   0x3A
#define F1         0x3B
#define F2         0x3C
#define F3         0x3D
#define F4         0x3E
#define F5         0x3F
#define F6         0x40
#define F7         0x41
#define F8         0x42
#define F9         0x43
#define F10        0x44
#define F11        0x57
#define F12        0x58
#define SPACE      0x39
#define DELETE     0x53

typedef struct {
    bool shift;
    bool ctrl;
    bool alt;
    bool capslock;
    char last_char;
    uint8_t last_scancode;
    bool key_pressed;
} keyboardvar;

extern keyboardvar kb;

void kb_init(void);
void kb_update(void);

#endif
