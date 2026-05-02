#include "ui/modal.h"

#include "lib/device.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"

#include "config/config.h"
#include "interrupts/idt.h"
#include "io/mouse.h"

#define MODAL_W 220
#define MODAL_H 110
#define MODAL_X ((SCREEN_WIDTH - MODAL_W) / 2)
#define MODAL_Y ((SCREEN_HEIGHT - MODAL_H) / 2)

#define MODAL_TITLE_H 16

#define MODAL_CONTENT_X (MODAL_X + 6)
#define MODAL_CONTENT_Y (MODAL_Y + MODAL_TITLE_H + 6)
#define MODAL_CONTENT_W (MODAL_W - 12)

#define MODAL_BTN_W 52
#define MODAL_BTN_H 13
#define MODAL_BTN_Y (MODAL_Y + MODAL_H - MODAL_BTN_H - 8)

#define MODAL_OK_X (MODAL_X + (MODAL_W - MODAL_BTN_W) / 2)

#define MODAL_CONF_OK_X (MODAL_X + MODAL_W / 2 - MODAL_BTN_W - 5)
#define MODAL_CONF_CNCL_X (MODAL_X + MODAL_W / 2 + 5)

#define MODAL_LINE_CHARS ((MODAL_CONTENT_W) / 5)
#define MODAL_LINE_H 10
#define MODAL_MAX_LINES 4

typedef struct {
    bool active;
    modal_type type;
    char title[64];
    char message[256];
    modal_cb on_confirm;
    modal_cb on_cancel;

    char lines[MODAL_MAX_LINES][80];
    int line_count;

    bool dismissed;
} modal_state_t;

static modal_state_t s;

/* ── helpers ────────────────────────────────────────────────────────────── */

static uint8_t bar_color(void) {
    switch (s.type) {
    case MODAL_ERROR:
        return RED;
    case MODAL_WARNING:
        return YELLOW;
    case MODAL_CONFIRM:
        return LIGHT_BLUE;
    default:
        return DARK_GRAY;
    }
}

static uint8_t bar_fg(void) {
    return (s.type == MODAL_WARNING) ? BLACK : WHITE;
}

static uint8_t btn_ok_color(void) {
    return (s.type == MODAL_ERROR) ? RED : LIGHT_BLUE;
}

static void split_message(const char *msg) {
    s.line_count = 0;
    int max_chars = MODAL_LINE_CHARS;
    if (max_chars > 78)
        max_chars = 78;

    const char *p = msg;
    while (*p && s.line_count < MODAL_MAX_LINES) {
        int nl = -1;
        for (int i = 0; i < max_chars; i++) {
            if (p[i] == '\0') {
                nl = i;
                break;
            }
            if (p[i] == '\n') {
                nl = i;
                break;
            }
        }

        if (nl < 0) {
            nl = max_chars;
            for (int i = max_chars; i > 0; i--) {
                if (p[i - 1] == ' ') {
                    nl = i - 1;
                    break;
                }
            }
        }

        int copy = nl < max_chars ? nl : max_chars;
        if (copy > 78)
            copy = 78;
        for (int i = 0; i < copy; i++)
            s.lines[s.line_count][i] = p[i];
        s.lines[s.line_count][copy] = '\0';
        s.line_count++;

        p += copy;
        if (*p == '\n')
            p++;
        while (*p == ' ')
            p++;
    }
}

static bool btn_hit(int bx, int by, int bw, int bh) {
    return mouse.left_clicked && mouse.x >= bx && mouse.x < bx + bw &&
           mouse.y >= by && mouse.y < by + bh;
}

static void draw_btn(int bx, int by, const char *label, uint8_t bg) {
    fill_rect(bx + 2, by + 2, MODAL_BTN_W, MODAL_BTN_H, BLACK);
    fill_rect(bx, by, MODAL_BTN_W, MODAL_BTN_H, bg);
    draw_rect(bx, by, MODAL_BTN_W, MODAL_BTN_H, BLACK);
    draw_line(bx + 1, by + 1, bx + MODAL_BTN_W - 2, by + 1, WHITE);
    draw_line(bx + 1, by + 1, bx + 1, by + MODAL_BTN_H - 2, WHITE);

    int lw = (int)strlen(label) * 5;
    int tx = bx + (MODAL_BTN_W - lw) / 2;
    int ty = by + (MODAL_BTN_H - 6) / 2;
    draw_string(tx, ty, (char *)label, WHITE, 2);
}

void modal_show(modal_type type, const char *title, const char *message,
                modal_cb on_confirm, modal_cb on_cancel) {
    s.active = true;
    s.dismissed = false;
    s.type = type;
    s.on_confirm = on_confirm;
    s.on_cancel = on_cancel;

    strncpy(s.title, title ? title : "", 63);
    s.title[63] = '\0';
    strncpy(s.message, message ? message : "", 255);
    s.message[255] = '\0';

    split_message(s.message);

    if (type == MODAL_ERROR) {
        speaker_beep(120, 80);
        speaker_beep(100, 80);
        speaker_beep(80, 150);
    } else if (type == MODAL_WARNING) {
        speaker_beep(440, 60);
        speaker_beep(330, 120);
    }
}

bool modal_active(void) { return s.active; }

void modal_update(void) {
    if (!s.active || s.dismissed)
        return;

    int close_bx = MODAL_X + MODAL_W - 14;
    int close_by = MODAL_Y + 3;
    if (btn_hit(close_bx, close_by, 10, 10)) {
        s.dismissed = true;
        s.active = false;
        if (s.on_cancel)
            s.on_cancel();
        return;
    }

    if (s.type == MODAL_CONFIRM) {
        if (btn_hit(MODAL_CONF_OK_X, MODAL_BTN_Y, MODAL_BTN_W, MODAL_BTN_H)) {
            s.dismissed = true;
            s.active = false;
            if (s.on_confirm)
                s.on_confirm();
            return;
        }
        if (btn_hit(MODAL_CONF_CNCL_X, MODAL_BTN_Y, MODAL_BTN_W, MODAL_BTN_H)) {
            s.dismissed = true;
            s.active = false;
            if (s.on_cancel)
                s.on_cancel();
            return;
        }
    } else {
        if (btn_hit(MODAL_OK_X, MODAL_BTN_Y, MODAL_BTN_W, MODAL_BTN_H)) {
            s.dismissed = true;
            s.active = false;
            if (s.on_confirm)
                s.on_confirm();
            return;
        }
    }
}

void modal_draw(void) {
    if (!s.active)
        return;

    fill_rect(MODAL_X + 4, MODAL_Y + 4, MODAL_W, MODAL_H, BLACK);

    fill_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, LIGHT_GRAY);
    draw_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, BLACK);

    draw_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_TITLE_H, BLACK);
    fill_rect(MODAL_X+1, MODAL_Y+1, MODAL_W-2, MODAL_TITLE_H-1, bar_color());
    draw_line(MODAL_X, MODAL_Y + MODAL_TITLE_H, MODAL_X + MODAL_W - 1,
              MODAL_Y + MODAL_TITLE_H, BLACK);

    /* Title text */
    char full_title[80];
    {
        const char *pfx = "";
        switch (s.type) {
        case MODAL_ERROR:
            pfx = "[!] ";
            break;
        case MODAL_WARNING:
            pfx = "[?] ";
            break;
        case MODAL_CONFIRM:
            pfx = "[ ] ";
            break;
        default:
            pfx = "[i] ";
            break;
        }
        snprintf(full_title, sizeof(full_title), "%s%s", pfx, s.title);
    }
    draw_string(MODAL_X + 4, MODAL_Y + 4, full_title, bar_fg(), 2);

    /* Close button (X) in the title bar */
    int close_bx = MODAL_X + MODAL_W - 14;
    int close_by = MODAL_Y + 3;
    fill_rect(close_bx, close_by, 10, 10, RED);
    draw_rect(close_bx, close_by, 10, 10, BLACK);
    draw_string(close_bx + 3, close_by + 2, "X", WHITE, 2);

    /* ── content area ── */
    fill_rect(MODAL_X + 1, MODAL_Y + MODAL_TITLE_H + 1, MODAL_W - 2,
              MODAL_H - MODAL_TITLE_H - 1, LIGHT_GRAY);

    /* Message lines */
    for (int i = 0; i < s.line_count; i++) {
        int ly = MODAL_CONTENT_Y + i * MODAL_LINE_H;
        if (ly + MODAL_LINE_H > MODAL_BTN_Y - 4)
            break;
        draw_string(MODAL_CONTENT_X, ly, s.lines[i], BLACK, 2);
    }

    draw_line(MODAL_X + 1, MODAL_BTN_Y - 5, MODAL_X + MODAL_W - 2,
              MODAL_BTN_Y - 5, DARK_GRAY);

    /* ── buttons ── */
    if (s.type == MODAL_CONFIRM) {
        draw_btn(MODAL_CONF_OK_X, MODAL_BTN_Y, "OK", btn_ok_color());
        draw_btn(MODAL_CONF_CNCL_X, MODAL_BTN_Y, "Cancel", DARK_GRAY);
    } else {
        draw_btn(MODAL_OK_X, MODAL_BTN_Y, "OK", btn_ok_color());
    }
}
