#include "ui/modal.h"
#include "lib/primitive_graphics.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "lib/speaker.h"
#include "io/mouse.h"
#include "config/config.h"

#define MODAL_W         200
#define MODAL_H         90
#define MODAL_X         ((SCREEN_WIDTH  - MODAL_W) / 2)
#define MODAL_Y         ((SCREEN_HEIGHT - MODAL_H) / 2)

#define MODAL_BTN_W     50
#define MODAL_BTN_H     12
#define MODAL_BTN_Y     (MODAL_Y + MODAL_H - MODAL_BTN_H - 8)

#define MODAL_OK_X      (MODAL_X + (MODAL_W - MODAL_BTN_W) / 2)

#define MODAL_CONFIRM_OK_X      (MODAL_X + MODAL_W / 2 - MODAL_BTN_W - 6)
#define MODAL_CONFIRM_CANCEL_X  (MODAL_X + MODAL_W / 2 + 6)

typedef struct {
    bool       active;
    modal_type type;
    const char *title;
    const char *message;
    modal_cb   on_confirm;
    modal_cb   on_cancel;
} modal_state_t;

static modal_state_t s = { false, MODAL_INFO, NULL, NULL, NULL, NULL };

static uint8_t bar_color(void) {
    switch (s.type) {
        case MODAL_ERROR:   return RED;
        case MODAL_WARNING: return YELLOW;
        case MODAL_CONFIRM: return LIGHT_BLUE;
        default:            return LIGHT_GRAY;
    }
}

static uint8_t title_color(void) {
    switch (s.type) {
        case MODAL_WARNING: return BLACK;
        default:            return WHITE;
    }
}

static const char *type_prefix(void) {
    switch (s.type) {
        case MODAL_ERROR:   return "[!] ";
        case MODAL_WARNING: return "[?] ";
        case MODAL_CONFIRM: return "    ";
        default:            return "[i] ";
    }
}

static bool btn_hit(int bx, int by) {
    return mouse.left_clicked
        && mouse.x >= bx && mouse.x < bx + MODAL_BTN_W
        && mouse.y >= by && mouse.y < by + MODAL_BTN_H;
}

static void draw_btn(int bx, int by, const char *label, uint8_t bg) {
    fill_rect(bx, by, MODAL_BTN_W, MODAL_BTN_H, bg);
    draw_rect(bx, by, MODAL_BTN_W, MODAL_BTN_H, BLACK);
    int tx = bx + MODAL_BTN_W / 2 - (int)(strlen(label) * 5) / 2;
    int ty = by + MODAL_BTN_H / 2 - 3;
    draw_string(tx, ty, (char *)label, WHITE, 2);
}

static void draw_wrapped(const char *msg, int x, int y, int max_chars,
                         int line_h, uint8_t color) {
    char line[64];
    int  msg_len   = (int)strlen(msg);
    int  start     = 0;
    int  line_num  = 0;

    while (start < msg_len && line_num < 3) {
        int end = start + max_chars;

        int nl_pos = start;
        while (nl_pos < start + max_chars && nl_pos < msg_len && msg[nl_pos] != '\n') nl_pos++;

        if (nl_pos < msg_len && msg[nl_pos] == '\n') {
            end = nl_pos;
        } else if (end >= msg_len) {
            end = msg_len;
        } else {
            int tmp = end;
            while (tmp > start && msg[tmp] != ' ') tmp--;
            if (tmp > start) end = tmp;
        }

        int len = end - start;
        if (len > 63) len = 63;
        memcpy(line, msg + start, len);
        line[len] = '\0';

        draw_string(x, y + line_num * line_h, line, color, 2);
        line_num++;

        start = end;
        while (start < msg_len && (msg[start] == ' ' || msg[start] == '\n')) start++;
    }
}


void modal_show(modal_type type, const char *title, const char *message, modal_cb on_confirm, modal_cb on_cancel) {
    s.active     = true;
    s.type       = type;
    s.title      = title   ? title   : "";
    s.message    = message ? message : "";
    s.on_confirm = on_confirm;
    s.on_cancel  = on_cancel;

    if (type == MODAL_ERROR) {
        speaker_beep(120, 80);
        speaker_beep(100, 80);
        speaker_beep(80,  150);
    } else if (type == MODAL_WARNING) {
        speaker_beep(440, 60);
        speaker_beep(330, 120);
    }
}

bool modal_active(void) { return s.active; }

void modal_update(void) {
    if (!s.active) return;

    if (s.type == MODAL_CONFIRM) {
        if (btn_hit(MODAL_CONFIRM_OK_X, MODAL_BTN_Y)) {
            s.active = false;
            if (s.on_confirm) s.on_confirm();
            return;
        }
        if (btn_hit(MODAL_CONFIRM_CANCEL_X, MODAL_BTN_Y)) {
            s.active = false;
            if (s.on_cancel) s.on_cancel();
            return;
        }
    } else {
        if (btn_hit(MODAL_OK_X, MODAL_BTN_Y)) {
            s.active = false;
            if (s.on_confirm) s.on_confirm();
            return;
        }
    }
}

void modal_draw(void) {
    if (!s.active) return;

    fill_rect(MODAL_X + 3, MODAL_Y + 3, MODAL_W, MODAL_H, BLACK);

    fill_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, LIGHT_GRAY);
    draw_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, BLACK);

    fill_rect(MODAL_X, MODAL_Y, MODAL_W, 14, bar_color());

    char full_title[64];
    const char *pfx = type_prefix();
    int pi = 0, ti = 0, fi = 0;
    while (pfx[pi] && fi < 62)    full_title[fi++] = pfx[pi++];
    while (s.title[ti] && fi < 62) full_title[fi++] = s.title[ti++];
    full_title[fi] = '\0';

    draw_string(MODAL_X + 4, MODAL_Y + 4, full_title, title_color(), 2);

    draw_wrapped(s.message,
                 MODAL_X + 8, MODAL_Y + 20,
                 36, 10, BLACK);

    draw_line(MODAL_X, MODAL_BTN_Y - 5,
              MODAL_X + MODAL_W - 1, MODAL_BTN_Y - 5, DARK_GRAY);

    if (s.type == MODAL_CONFIRM) {
        draw_btn(MODAL_CONFIRM_OK_X,     MODAL_BTN_Y, "OK",     LIGHT_BLUE);
        draw_btn(MODAL_CONFIRM_CANCEL_X, MODAL_BTN_Y, "Cancel", DARK_GRAY);
    } else {
        uint8_t btn_bg = (s.type == MODAL_ERROR) ? RED : LIGHT_BLUE;
        draw_btn(MODAL_OK_X, MODAL_BTN_Y, "OK", btn_bg);
    }
}
