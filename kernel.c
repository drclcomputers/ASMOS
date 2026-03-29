#include "lib/utils.h"
#include "ui/ui.h"
#include "io/mouse.h"
#include "io/keyboard.h"

// A simple 11x16 arrow cursor bitmap (1 = draw, 0 = skip)
// Each row is 11 bits wide, stored as two bytes
static const uint16_t cursor_shape[16] = {
    0b11000000000,
    0b11100000000,
    0b11110000000,
    0b11111000000,
    0b11111100000,
    0b11111110000,
    0b11111111000,
    0b11111111100,
    0b11111110000,
    0b11101110000,
    0b11000111000,
    0b00000111000,
    0b00000011100,
    0b00000011100,
    0b00000001110,
    0b00000000000,
};

void draw_cursor(int x, int y) {
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 11; col++) {
            if (cursor_shape[row] & (1 << (10 - col))) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < 320 && py >= 0 && py < 200)
                    draw_dot(px, py, 0x00);   // black
            }
        }
    }
}

// A small fixed text buffer for keyboard input demo
#define INPUT_MAX 64
static char input_buf[INPUT_MAX + 1];
static int  input_len = 0;

void kmain(void) {
    mouse_init();
    kb_init();

    while (1) {
        // ── Input ──────────────────────────────────────────────────
        mouse_update();
        kb_update();

        // Append typed characters to input buffer
        if (kb.last_char && kb.last_char != '\n') {
            if (kb.last_char == '\b') {         // backspace
                if (input_len > 0)
                    input_buf[--input_len] = '\0';
            } else if (input_len < INPUT_MAX) {
                input_buf[input_len++] = kb.last_char;
                input_buf[input_len]   = '\0';
            }
        }

        // ── Draw ───────────────────────────────────────────────────
        clear_screen(0xFF);                     // white desktop

        // A window
        draw_window(20, 20, 280, 140, "My Window");

        // Show typed text inside the window
        draw_string(28, 44, input_buf, 0x00, 2);

        // Show mouse coordinates (bottom of screen)
        // itoa-lite: just show a dot at the cursor position for now
        draw_cursor(mouse.x, mouse.y);

        // Highlight something on left click
        if (mouse.left)
            fill_rect(1, 192, 60, 7, 0x00);    // black bar while held

        blit();
    }
}
