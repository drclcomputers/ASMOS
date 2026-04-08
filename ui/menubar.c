#include "ui/menubar.h"
#include "lib/primitive_graphics.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "io/mouse.h"
#include "config/config.h"
#include "lib/time.h"
#include "os/os.h"

#define BAR_BG      LIGHT_GRAY
#define BAR_FG      WHITE
#define DROP_BG     CYAN
#define DROP_FG     WHITE
#define DROP_SEL    RED
#define DROP_SEP    WHITE
#define DROP_DIS    WHITE
#define PADDING     4
#define CLOCK_CHARS 8
#define CLOCK_W     (CLOCK_CHARS * 5 + PADDING * 2)

extern app_descriptor clock_app;

void menubar_init(void) {
    memset(&g_menubar, 0, sizeof(menubar));
    g_menubar.open_index = -1;
}

menu* menubar_add_menu(menubar *mb, char *title) {
    if (mb->menu_count >= MAX_MENUS) return 0;
    menu *m = &mb->menus[mb->menu_count++];
    memset(m, 0, sizeof(menu));
    m->title = title;
    m->open = false;
    return m;
}

void menu_add_item(menu *m, char *label, MenuAction action) {
    if (m->item_count >= MAX_MENU_ITEMS) return;
    menu_item *it = &m->items[m->item_count++];
    it->label = label;
    it->action = action;
    it->disabled = false;
}

void menu_add_separator(menu *m) {
    if (m->item_count >= MAX_MENU_ITEMS) return;
    menu_item *it  = &m->items[m->item_count++];
    it->label = 0;
    it->action = 0;
    it->disabled = true;
}

void menubar_close_all(menubar *mb) {
    for (int i = 0; i < mb->menu_count; i++)
        mb->menus[i].open = false;
    mb->open_index = -1;
}

void menubar_layout(menubar *mb) {
    int cursor_x = 2 + CLOCK_W;
    for (int i = 0; i < mb->menu_count; i++) {
        menu *m = &mb->menus[i];
        int tw = (int)strlen(m->title) * 5 + PADDING * 2;
        m->bar_x = cursor_x;
        m->bar_w = tw;
        cursor_x += tw + 2;
    }
}

static void write_2digit(char *buf, uint8_t val) {
    buf[0] = '0' + (val / 10);
    buf[1] = '0' + (val % 10);
}

static void build_clock_str(char *out) {
    time_full_t t = time_rtc_local();
    write_2digit(out + 0, t.hours);
    out[2] = ':';
    write_2digit(out + 3, t.minutes);
    out[5] = ':';
    write_2digit(out + 6, t.seconds);
    out[8] = '\0';
}

void menubar_draw(menubar *mb) {
    fill_rect(0, 0, SCREEN_WIDTH, MENUBAR_H_SIZE, BAR_BG);

    int cursor_x = 2;

    char clock_str[9];
    build_clock_str(clock_str);
    draw_string(cursor_x, 2, clock_str, BAR_FG, 2);
    cursor_x += CLOCK_W;

    for (int i = 0; i < mb->menu_count; i++) {
        menu *m = &mb->menus[i];
        int tw = (int)strlen(m->title) * 5 + PADDING * 2;

        m->bar_x = cursor_x;
        m->bar_w = tw;

        if (m->open) fill_rect(cursor_x, 0, tw, MENUBAR_H_SIZE, DROP_SEL);

        draw_string(cursor_x + PADDING, 2, m->title, BAR_FG, 2);
        cursor_x += tw + 2;

        if (m->open) {
            int dw = MENU_ITEM_MIN_W;
            for (int j = 0; j < m->item_count; j++) {
                if (!m->items[j].label) continue;
                int iw = (int)strlen(m->items[j].label) * 5 + PADDING * 2 + 4;
                if (iw > dw) dw = iw;
            }

            int dy = MENUBAR_H_SIZE;
            int dh = m->item_count * MENU_ITEM_H;

            fill_rect(m->bar_x + 2, dy + 2, dw, dh, BLACK);

            fill_rect(m->bar_x, dy, dw, dh, DROP_BG);
            draw_rect(m->bar_x, dy, dw, dh, BLACK);

            for (int j = 0; j < m->item_count; j++) {
                menu_item *it = &m->items[j];
                int iy = dy + j * MENU_ITEM_H;

                if (!it->label) {
                    int sy = iy + MENU_ITEM_H / 2;
                    draw_string(m->bar_x + 2, sy, "---", DROP_SEP, 2);
                    continue;
                }

                bool hovered = mouse.x >= m->bar_x &&
                               mouse.x <  m->bar_x + dw &&
                               mouse.y >= iy &&
                               mouse.y <  iy + MENU_ITEM_H;

                if (hovered && !it->disabled)
                    fill_rect(m->bar_x + 1, iy, dw - 2, MENU_ITEM_H, DROP_SEL);

                unsigned char fg = it->disabled ? DROP_DIS : DROP_FG;
                draw_string(m->bar_x + PADDING, iy + 2, it->label, fg, 2);
            }
        }
    }
}

void menubar_update(menubar *mb) {
    if (mouse.left_clicked && mouse.y < MENUBAR_H_SIZE) {
        g_menubar_click_consumed = true;

        if (mouse.x < 2 + CLOCK_W) {
            os_launch_app(&clock_app);
            return;
        }

        for (int i = 0; i < mb->menu_count; i++) {
            menu *m = &mb->menus[i];
            if (mouse.x >= m->bar_x && mouse.x < m->bar_x + m->bar_w) {
                if (m->open) {
                    menubar_close_all(mb);
                } else {
                    menubar_close_all(mb);
                    m->open = true;
                    mb->open_index = i;
                }
                return;
            }
        }
        menubar_close_all(mb);
        return;
    }

    if (mb->open_index >= 0 && mouse.left_clicked) {
        menu *m = &mb->menus[mb->open_index];
        int dy = MENUBAR_H_SIZE;

        int dw = MENU_ITEM_MIN_W;
        for (int j = 0; j < m->item_count; j++) {
            if (!m->items[j].label) continue;
            int iw = (int)strlen(m->items[j].label) * 5 + PADDING * 2 + 4;
            if (iw > dw) dw = iw;
        }

        if (mouse.x >= m->bar_x && mouse.x < m->bar_x + dw &&
            mouse.y >= dy) {
            g_menubar_click_consumed = true;

            int idx = (mouse.y - dy) / MENU_ITEM_H;
            if (idx >= 0 && idx < m->item_count) {
                menu_item *it = &m->items[idx];
                if (it->label && !it->disabled && it->action) {
                    it->action();
                }
            }
            menubar_close_all(mb);
        } else {
            g_menubar_click_consumed = true;
            menubar_close_all(mb);
        }
        return;
    }

    if (mb->open_index >= 0 && mouse.y < MENUBAR_H_SIZE) {
        for (int i = 0; i < mb->menu_count; i++) {
            menu *m = &mb->menus[i];
            if (i == mb->open_index) continue;
            if (mouse.x >= m->bar_x && mouse.x < m->bar_x + m->bar_w) {
                menubar_close_all(mb);
                m->open        = true;
                mb->open_index = i;
                return;
            }
        }
    }
}
