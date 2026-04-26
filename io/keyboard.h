#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "lib/core.h"

#define ESC 0x01
#define BACKSPACE 0x0E
#define TAB 0x0F
#define ENTER 0x1C

#define LCTRL 0x1D
#define LSHIFT 0x2A
#define RSHIFT 0x36
#define LALT 0x38
#define CAPSLOCK 0x3A

#define F1 0x3B
#define F2 0x3C
#define F3 0x3D
#define F4 0x3E
#define F5 0x3F
#define F6 0x40
#define F7 0x41
#define F8 0x42
#define F9 0x43
#define F10 0x44
#define F11 0x57
#define F12 0x58

#define SPACE 0x39
#define UP_ARROW 0x48
#define DOWN_ARROW 0x50
#define LEFT_ARROW 0x4B
#define RIGHT_ARROW 0x4D

#define HOME 0x47
#define END 0x4F
#define PAGE_UP 0x49
#define PAGE_DOWN 0x51
#define DELETE 0x53
#define INSERT 0x52

#define NUM_LOCK 0x45
#define SCROLL_LOCK 0x46
#define PRINT_SCREEN 0x37
#define PAUSE 0x45

#define MINUS 0x0C
#define EQUALS 0x0D
#define LBRACKET 0x1A
#define RBRACKET 0x1B
#define SEMICOLON 0x27
#define APOSTROPHE 0x28
#define GRAVE 0x29
#define BACKSLASH 0x2B
#define COMMA 0x33
#define PERIOD 0x34
#define SLASH 0x35

#define A_KEY 0x1E
#define B_KEY 0x30
#define C_KEY 0x2E
#define D_KEY 0x20
#define E_KEY 0x12
#define F_KEY 0x21
#define G_KEY 0x22
#define H_KEY 0x23
#define I_KEY 0x17
#define J_KEY 0x24
#define K_KEY 0x25
#define L_KEY 0x26
#define M_KEY 0x32
#define N_KEY 0x31
#define O_KEY 0x18
#define P_KEY 0x19
#define Q_KEY 0x10
#define R_KEY 0x13
#define S_KEY 0x1F
#define T_KEY 0x14
#define U_KEY 0x16
#define V_KEY 0x2F
#define W_KEY 0x11
#define X_KEY 0x2D
#define Y_KEY 0x15
#define Z_KEY 0x2C

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
void kb_process_byte(uint8_t raw);

#endif
