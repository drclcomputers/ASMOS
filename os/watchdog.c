// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "os/watchdog.h"

#include "config/config.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/pointer.h"
#include "drivers/ps2.h"
#include "drivers/sermouse.h"
#include "lib/graphics.h"
#include "lib/io.h"
#include "lib/time.h"
#include "os/appguard.h"
#include "ui/cursor.h"

/* hourglass, 9x11: B black, W white, . see-through */
#define HG_W 9
#define HG_H 11
static const char *const k_glass[HG_H] = {
    "BBBBBBBBB", "BWWWWWWWB", ".BWBBBWB.", ".BWWBWWB.",
    "..BWBWB..", "...BWB...", "..BWWWB..", ".BWWBWWB.",
    ".BWBBBWB.", "BWWWWWWWB", "BBBBBBBBB",
};
static uint8_t s_glass[HG_W * HG_H];

static volatile uint32_t s_beat; /* last heartbeat (ms); 0 = GUI not up */
static volatile bool s_gui;      /* the desktop runs: input from the IRQ */
static volatile bool s_shown;    /* hourglass is the pointer */
static volatile bool s_monitor_req;

static void glass_init(void) {
    if (s_glass[0])
        return;
    for (int i = 0; i < HG_W * HG_H; i++) {
        char c = k_glass[i / HG_W][i % HG_W];
        s_glass[i] = c == 'B' ? BLACK : c == 'W' ? WHITE : 0xFF;
    }
}

/* the hourglass takes the arrow's place (same pointer: the arrow cannot
 * stay behind) */
static void glass_show(void) {
    glass_init();
    s_shown = true;
    pointer_shape(s_glass, HG_W, HG_H, HG_W / 2, HG_H / 2);
    const mousevar *m = mouse_live();
    pointer_move(m->x, m->y);
    pointer_show(true);
}

void wd_heartbeat(void) {
    s_beat = time_millis();
    if (!s_beat)
        s_beat = 1;
    if (!s_gui) { /* the desktop starts: the timer reads the mouse */
        s_gui = true;
        ps2_irq_mode(true);
    }
    if (s_shown) { /* the arrow comes back */
        s_shown = false;
        cursor_restore();
    }
}

bool wd_busy_shown(void) { return s_shown; }

void wd_stop(void) {
    s_beat = 0;
    s_gui = false;
    ps2_irq_mode(false);
    s_shown = false;
    pointer_show(false); /* no pointer outside the desktop */
}

void wd_busy(void) {
    if (!s_beat)
        return;
    glass_show();
    s_beat = time_millis() - WD_BUSY_MS; /* the IRQ keeps it following */
}

bool wd_take_monitor_request(void) {
    bool r = s_monitor_req;
    s_monitor_req = false;
    return r;
}

/* the keys, while the loop is stuck */
static void hotkey(void) {
    uint8_t sc = kb.last_scancode;
    bool end_app = (kb.win || kb.ctrl) && kb.shift && sc == Q_KEY;
    bool monitor =
        kb.ctrl && kb.alt && (sc == LWIN_KEY || sc == RWIN_KEY || sc == DELETE);
    if (!end_app && !monitor)
        return;
    kb.key_pressed = false; /* not for the app when the loop comes back */
    if (monitor)
        s_monitor_req = true;
    if (appguard_active()) /* does not return */
        appguard_abort(end_app ? APPFAULT_KILLED : APPFAULT_STOPPED);
}

/* the keys, while the loop is stuck (the app gets none of them later) */
static void stuck_keys(void) {
    int b;
    while ((b = ps2_key_byte()) >= 0) {
        kb.key_pressed = false;
        kb_process_byte((uint8_t)b);
        if (kb.key_pressed)
            hotkey();
    }
}

void wd_tick(void) {
    static uint32_t n;
    static int px = -1, py = -1;
    if (!s_gui)
        return;
    ps2_irq_poll(); /* mouse bytes now, keys into the ring */
    n++;
    if (s_beat && n % 20 == 0 && time_millis() - s_beat >= WD_BUSY_MS) {
        stuck_keys(); /* may abort the stuck app: does not return */
        glass_show();
    }
    if (n % 4 == 0) { /* the pointer follows the mouse, at most 250/s */
        const mousevar *m = mouse_live();
        if (m->x != px || m->y != py) {
            px = m->x;
            py = m->y;
            pointer_move(px, py);
        }
    }
}
