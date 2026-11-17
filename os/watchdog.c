#include "os/watchdog.h"

#include "config/config.h"
#include "drivers/gpu.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
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
static volatile bool s_shown;    /* hourglass on screen */
static int s_gx, s_gy;           /* where it is */
static volatile bool s_monitor_req;

static void glass_init(void) {
    if (s_glass[0])
        return;
    for (int i = 0; i < HG_W * HG_H; i++) {
        char c = k_glass[i / HG_W][i % HG_W];
        s_glass[i] = c == 'B' ? BLACK : c == 'W' ? WHITE : 0xFF;
    }
}

static void glass_show(void) {
    int x = mouse.x - HG_W / 2, y = mouse.y - HG_H / 2;
    if (s_shown && x == s_gx && y == s_gy)
        return;
    glass_init();
    if (s_shown)
        gpu_overlay_clear(s_gx, s_gy, HG_W, HG_H);
    else /* the arrow comes off the screen: only the hourglass is shown */
        cursor_erase_screen();
    s_gx = x;
    s_gy = y;
    gpu_overlay(x, y, HG_W, HG_H, s_glass);
    s_shown = true;
}

void wd_heartbeat(void) {
    s_beat = time_millis();
    if (!s_beat)
        s_beat = 1;
    if (s_shown) { /* the next blit repaints what the hourglass covered */
        s_shown = false;
        gpu_overlay_forget();
        gpu_invalidate();
    }
}

bool wd_busy_shown(void) { return s_shown; }

void wd_stop(void) {
    s_beat = 0;
    if (s_shown) { /* take the hourglass off: the next blit is whole */
        s_shown = false;
        gpu_overlay_forget();
        gpu_invalidate();
    }
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

/* what ps2_update does, minus clearing the per-frame flags */
static void poll_input(void) {
    for (int n = 0; n < 64; n++) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01))
            break;
        uint8_t b = inb(0x60);
        if (st & 0x20) {
            mouse_process_byte(b);
        } else {
            kb.key_pressed = false;
            kb_process_byte(b);
            if (kb.key_pressed)
                hotkey();
        }
    }
    sermouse_poll();
}

void wd_tick(void) {
    static uint32_t n;
    if (++n % 20 || !s_beat) /* 50 times a second, GUI only */
        return;
    if (time_millis() - s_beat < WD_BUSY_MS)
        return;
    poll_input();
    glass_show();
}
