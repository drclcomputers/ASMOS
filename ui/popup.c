#include "ui/popup.h"

#include "config/config.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "lib/graphics.h"
#include "lib/string.h"
#include "os/os.h"
#include "ui/window.h"

#define POP_BG CYAN
#define POP_FG WHITE
#define POP_SEL RED
#define POP_DIS LIGHT_GRAY
#define PAD 4
#define SEP_H 5

static menu s_menu;
static bool s_open;
static int s_x, s_y, s_w, s_h;
static int s_hot = -1;           /* highlighted item */
static int s_mx = -1, s_my = -1; /* last mouse position seen */
static const window *s_owner;    /* focused window when shown */

menu *popup_menu(void) {
    memset(&s_menu, 0, sizeof(s_menu));
    s_menu.title = "";
    s_open = false;
    return &s_menu;
}

static int item_h(const menu_item *it) {
    return it->label ? MENU_ITEM_H : SEP_H;
}

static bool usable(int i) {
    return i >= 0 && i < s_menu.item_count && s_menu.items[i].label &&
           !s_menu.items[i].disabled;
}

void popup_show(int x, int y) {
    if (!s_menu.item_count)
        return;
    s_w = MENU_ITEM_MIN_W;
    s_h = 2;
    for (int i = 0; i < s_menu.item_count; i++) {
        const menu_item *it = &s_menu.items[i];
        s_h += item_h(it);
        if (it->label) {
            int w = (int)strlen(it->label) * 5 + PAD * 2 + 4;
            if (w > s_w)
                s_w = w;
        }
    }
    /* keep on screen (and off the menubar); flip above/left if needed */
    if (x + s_w + 2 > SCREEN_WIDTH)
        x = x - s_w >= 0 ? x - s_w : SCREEN_WIDTH - s_w - 2;
    if (y + s_h + 2 > SCREEN_HEIGHT)
        y = y - s_h >= MENUBAR_H ? y - s_h : SCREEN_HEIGHT - s_h - 2;
    if (y < MENUBAR_H)
        y = MENUBAR_H;
    if (x < 0)
        x = 0;
    s_x = x;
    s_y = y;
    s_hot = -1;
    s_mx = mouse.x;
    s_my = mouse.y;
    s_owner = wm_focused_window();
    s_open = true;
    menubar_close_all(&g_menubar);
}

void popup_show_for(const control *c) {
    if (c)
        popup_show(c->ax + c->mx, c->ay + c->my);
}

bool popup_is_open(void) { return s_open; }

void popup_close(void) { s_open = false; }

void popup_window_gone(const window *w) {
    if (s_open && w && w == s_owner)
        s_open = false;
}

static int item_at(int mx, int my) {
    if (mx < s_x || mx >= s_x + s_w)
        return -1;
    int y = s_y + 1;
    for (int i = 0; i < s_menu.item_count; i++) {
        int h = item_h(&s_menu.items[i]);
        if (my >= y && my < y + h)
            return i;
        y += h;
    }
    return -1;
}

static void run(int i) {
    menu_item it = s_menu.items[i]; /* the action may build a new popup */
    s_open = false;
    menu_item_run(&it);
}

static void step(int dir) {
    int n = s_menu.item_count;
    int i = s_hot;
    for (int k = 0; k < n; k++) {
        i = (i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n);
        if (usable(i)) {
            s_hot = i;
            return;
        }
    }
}

void popup_update(void) {
    if (!s_open)
        return;
    if (g_menubar.open_index >= 0) {
        s_open = false;
        return;
    }
    /* hover follows the mouse only when it moves (keys can steer too) */
    if (mouse.x != s_mx || mouse.y != s_my) {
        s_mx = mouse.x;
        s_my = mouse.y;
        int i = item_at(mouse.x, mouse.y);
        s_hot = usable(i) ? i : -1;
    }
    if (mouse.left_clicked || mouse.right_clicked) {
        bool in = mouse.x >= s_x && mouse.x < s_x + s_w && mouse.y >= s_y &&
                  mouse.y < s_y + s_h;
        int i = item_at(mouse.x, mouse.y);
        bool left = mouse.left_clicked;
        if (!in && !left) { /* right-click elsewhere: that one's menu */
            s_open = false;
            return;
        }
        mouse.left_clicked = false;
        mouse.right_clicked = false;
        g_menubar_click_consumed = true;
        if (!in)
            s_open = false; /* a left click elsewhere just closes */
        else if (left && usable(i))
            run(i);
        return;
    }
    if (!kb.key_pressed)
        return;
    uint8_t sc = kb.last_scancode;
    kb.key_pressed = false; /* the popup owns the keyboard */
    if (sc == ESC) {
        s_open = false;
    } else if (sc == UP_ARROW) {
        step(-1);
    } else if (sc == DOWN_ARROW) {
        step(1);
    } else if (sc == HOME) {
        s_hot = -1;
        step(1);
    } else if (sc == END) {
        s_hot = -1;
        step(-1);
    } else if (sc == ENTER || sc == SPACE) {
        if (usable(s_hot))
            run(s_hot);
    } else if (kb.last_char > ' ') { /* first letter picks an item */
        char ch = (char)(kb.last_char | 0x20);
        for (int k = 1; k <= s_menu.item_count; k++) {
            int i = (s_hot + k + s_menu.item_count) % s_menu.item_count;
            if (usable(i) && (s_menu.items[i].label[0] | 0x20) == ch) {
                s_hot = i;
                break;
            }
        }
    }
}

void popup_draw(void) {
    if (!s_open)
        return;
    gfx_reset_clip();
    fill_rect(s_x + 2, s_y + 2, s_w, s_h, BLACK); /* shadow */
    fill_rect(s_x, s_y, s_w, s_h, POP_BG);
    draw_rect(s_x, s_y, s_w, s_h, BLACK);
    int y = s_y + 1;
    for (int i = 0; i < s_menu.item_count; i++) {
        const menu_item *it = &s_menu.items[i];
        if (!it->label) {
            draw_line(s_x + 3, y + 2, s_x + s_w - 4, y + 2, POP_FG);
            y += SEP_H;
            continue;
        }
        if (i == s_hot)
            fill_rect(s_x + 1, y, s_w - 2, MENU_ITEM_H, POP_SEL);
        draw_string(s_x + PAD, y + 2, it->label,
                    it->disabled ? POP_DIS : POP_FG, 2);
        y += MENU_ITEM_H;
    }
}
