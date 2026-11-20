#include "drivers/keyboard.h"
#include "lib/core.h"

#define KEY_RELEASE 0x80

keyboardvar kb;

static const char lower[128] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9',  '0', '-', '=',  0,
    0,   'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',  '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z',
    'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*',  0,   ' ', 0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    '7', '8', '9',  '-',
    '4', '5', '6', '+', '1', '2', '3', '0', '.', 0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,
};

static const char upper[128] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',  0,
    0,   'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|',  'Z',
    'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '7', '8', '9',  '-',
    '4', '5', '6', '+', '1', '2', '3', '0', '.', 0,   0,   0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,
};

static bool extended = false;

/* Alt + digits on the number pad: the character with that code (0-255,
 * the code page's), typed when Alt comes up, as in DOS and Windows */
static int s_alt_code = -1;

static int numpad_digit(uint8_t sc) {
    static const uint8_t pad[10] = {0x52, 0x4F, 0x50, 0x51, 0x4B,
                                    0x4C, 0x4D, 0x47, 0x48, 0x49};
    for (int i = 0; i < 10; i++)
        if (pad[i] == sc)
            return i;
    return -1;
}

void kb_init(void) {
    while (inb(0x64) & 0x01)
        inb(0x60);
}

void kb_update(void) {
    kb.last_char = 0;
    kb.last_scancode = 0;
    kb.key_pressed = false;
    kb.ctrl_c = false;
    kb.ctrl_x = false;
    kb.ctrl_v = false;
    kb.ctrl_a = false;
    kb.ctrl_s = false;
    kb.ctrl_z = false;
    kb.ctrl_q = false;
    kb.ctrl_n = false;
    kb.ctrl_o = false;
    kb.ctrl_f = false;
    kb.ctrl_shift_c = false;
    kb.ctrl_shift_v = false;
    kb.ctrl_shift_z = false;
    kb.ctrl_shift_s = false;
}

void kb_process_byte(uint8_t raw) {
    if (raw == 0xE0) {
        extended = true;
        return;
    }

    bool released = (raw & KEY_RELEASE) != 0;
    uint8_t sc = raw & ~KEY_RELEASE;

    bool ext = extended;
    extended = false;

    if (sc == LSHIFT || sc == RSHIFT) {
        kb.shift = !released;
        return;
    }
    if (sc == LCTRL) {
        kb.ctrl = !released;
        return;
    }
    if (sc == LALT) {
        kb.alt = !released;
        if (!released) {
            s_alt_code = -1;
        } else if (s_alt_code > 0) {
            kb.last_char = (char)(s_alt_code & 0xFF);
            kb.last_scancode = 0;
            kb.key_pressed = true;
            s_alt_code = -1;
        }
        return;
    }
    if (kb.alt && !ext && numpad_digit(sc) >= 0) {
        if (!released)
            s_alt_code =
                (s_alt_code < 0 ? 0 : s_alt_code) * 10 + numpad_digit(sc);
        if (s_alt_code > 999)
            s_alt_code %= 1000;
        return;
    }
    if (ext && (sc == LWIN_KEY || sc == RWIN_KEY)) {
        kb.win = !released;
        return;
    }

    if (sc == CAPSLOCK && !released) {
        kb.capslock = !kb.capslock;
        return;
    }

    if (released)
        return;

    kb.last_scancode = sc;
    kb.key_pressed = true;

    if (kb.ctrl) {
        kb.ctrl_c = (sc == C_KEY);
        kb.ctrl_x = (sc == X_KEY);
        kb.ctrl_v = (sc == V_KEY);
        kb.ctrl_a = (sc == A_KEY);
        kb.ctrl_s = (sc == S_KEY);
        kb.ctrl_z = (sc == Z_KEY);
        kb.ctrl_q = (sc == Q_KEY);
        kb.ctrl_n = (sc == N_KEY);
        kb.ctrl_o = (sc == O_KEY);
        kb.ctrl_f = (sc == F_KEY);
        kb.ctrl_shift_c = (kb.shift && sc == C_KEY);
        kb.ctrl_shift_v = (kb.shift && sc == V_KEY);
        kb.ctrl_shift_z = (kb.shift && sc == Z_KEY);
        kb.ctrl_shift_s = (kb.shift && sc == S_KEY);
    }

    if (ext) {
        kb.last_char = (sc == ENTER) ? '\n' : (sc == SLASH) ? '/' : 0;
        return;
    }
    bool use_upper = kb.shift ^ kb.capslock;
    if (sc < 128)
        kb.last_char = use_upper ? upper[sc] : lower[sc];
}
