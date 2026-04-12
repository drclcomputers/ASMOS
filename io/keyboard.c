#include "io/keyboard.h"
#include "lib/core.h"

#define KEY_RELEASE 0x80

keyboardvar kb = { 0, 0, 0, 0, 0, 0, 0 };

static const char lower[128] = {
  0,    0,   '1', '2', '3', '4', '5', '6',
 '7', '8',  '9', '0', '-', '=',   0,   0,
 'q', 'w',  'e', 'r', 't', 'y', 'u', 'i',
 'o', 'p',  '[', ']', '\n', 0,  'a', 's',
 'd', 'f',  'g', 'h', 'j', 'k', 'l', ';',
 '\'', '`',   0, '\\', 'z', 'x', 'c', 'v',
 'b', 'n',  'm', ',', '.', '/',   0,  '*',
  0,  ' ',   0,   0,   0,   0,   0,   0,
  0,   0,    0,   0,   0,   0,   0,  '7',
 '8', '9',  '-', '4', '5', '6', '+', '1',
 '2', '3',  '0', '.',  0,   0,   0,   0,
  0,   0,    0,   0,   0,   0,   0,   0,
  0,   0,    0,   0,   0,   0,   0,   0,
  0,   0,    0,   0,   0,   0,   0,   0,
  0,   0,    0,   0,   0,   0,   0,   0,
  0,   0,    0,   0,   0,   0,   0,   0,
};

static const char upper[128] = {
  0,    0,   '!', '@', '#', '$', '%', '^',
 '&', '*',  '(', ')', '_', '+',   0,   0,
 'Q', 'W',  'E', 'R', 'T', 'Y', 'U', 'I',
 'O', 'P',  '{', '}', '\n', 0,  'A', 'S',
 'D', 'F',  'G', 'H', 'J', 'K', 'L', ':',
 '"', '~',  0,  '|', 'Z', 'X', 'C', 'V',
 'B', 'N',  'M', '<', '>', '?',   0,  '*',
  0,  ' ',  0,   0,   0,   0,   0,   0,
  0,   0,   0,   0,   0,   0,   0,  '7',
 '8', '9', '-', '4', '5', '6', '+', '1',
 '2', '3', '0', '.',  0,   0,   0,   0,
  0,   0,   0,   0,   0,   0,   0,   0,
  0,   0,   0,   0,   0,   0,   0,   0,
  0,   0,   0,   0,   0,   0,   0,   0,
  0,   0,   0,   0,   0,   0,   0,   0,
  0,   0,   0,   0,   0,   0,   0,   0,
};

static bool extended = false;

void kb_init(void) {
    while (inb(0x64) & 0x01)
        inb(0x60);
}

void kb_update(void) {
    kb.last_char     = 0;
    kb.last_scancode = 0;
    kb.key_pressed   = false;
}

void kb_process_byte(uint8_t raw) {
    if (raw == 0xE0) {
        extended = true;
        return;
    }

    bool released = (raw & KEY_RELEASE) != 0;
    uint8_t sc    = raw & ~KEY_RELEASE;

    bool ext  = extended;
    extended  = false;

    //if (ext) return;

    if (sc == LSHIFT || sc == RSHIFT) { kb.shift = !released; return; }
    if (sc == LCTRL)                  { kb.ctrl = !released; return; }
    if (sc == LALT)                   { kb.alt = !released; return; }

    if (sc == CAPSLOCK && !released) {
        kb.capslock = !kb.capslock;
        return;
    }

    if (released) return;

    kb.last_scancode = sc;
    kb.key_pressed   = true;

    bool use_upper = kb.shift ^ kb.capslock;
    if (sc < 128)
        kb.last_char = use_upper ? upper[sc] : lower[sc];
}
