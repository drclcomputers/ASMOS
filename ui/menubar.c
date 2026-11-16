#include "ui/menubar.h"
#include "config/runtime_config.h"
#include "ui/uifont.h"

#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "os/os.h"
#include "ui/window.h"

#include "config/config.h"
#include "drivers/mouse.h"
#include "os/os.h"

#define BAR_BG LIGHT_GRAY
#define BAR_FG WHITE
#define DROP_BG CYAN
#define DROP_FG WHITE
#define DROP_SEL RED
#define DROP_SEP WHITE
#define DROP_DIS WHITE
#define PADDING 4
#define CLOCK_CHARS 8
#define CLOCK_W (CLOCK_CHARS * 5 + PADDING * 2)

extern app_descriptor clock_app;

void menubar_init(void) {
    memset(&g_menubar, 0, sizeof(menubar));
    g_menubar.open_index = -1;
    g_menubar.sub_item = -1;
    g_menubar.prev_x = g_menubar.next_x = -1;
}

menu *menubar_add_menu(menubar *mb, char *title) {
    if (mb->menu_count >= MAX_MENUS)
        return 0;
    menu *m = &mb->menus[mb->menu_count++];
    memset(m, 0, sizeof(menu));
    m->title = title;
    m->open = false;
    return m;
}

void (*g_menu_call_hook)(void *state, MenuActionUD fn, void *user);

void menu_add_item(menu *m, char *label, MenuAction action) {
    if (m->item_count >= MAX_MENU_ITEMS)
        return;
    menu_item *it = &m->items[m->item_count++];
    it->label = label;
    it->sub = NULL;
    it->action = action;
    it->action_ud = NULL;
    it->dyn_state = NULL;
    it->disabled = false;
}

void menu_add_item_ud(menu *m, char *label, MenuActionUD action,
                      void *userdata) {
    if (m->item_count >= MAX_MENU_ITEMS)
        return;
    menu_item *it = &m->items[m->item_count++];
    it->label = label;
    it->sub = NULL;
    it->action = NULL;
    it->action_ud = action;
    it->userdata = userdata;
    it->dyn_state = NULL;
    it->disabled = false;
}

menu *menu_add_submenu(menu *m, char *label) {
    if (!m || m->is_sub || m->item_count >= MAX_MENU_ITEMS)
        return NULL;
    menu *sub = kzalloc(sizeof(menu));
    if (!sub)
        return NULL;
    sub->title = label;
    sub->is_sub = true;
    menu_item *it = &m->items[m->item_count++];
    memset(it, 0, sizeof(*it));
    it->label = label;
    it->sub = sub;
    return sub;
}

void menu_free_subs(menu *m) {
    for (int i = 0; i < m->item_count; i++)
        if (m->items[i].sub) {
            kfree(m->items[i].sub);
            m->items[i].sub = NULL;
        }
}

void menu_clear(menu *m) {
    menu_free_subs(m);
    m->item_count = 0;
}

void menu_add_separator(menu *m) {
    if (m->item_count >= MAX_MENU_ITEMS)
        return;
    menu_item *it = &m->items[m->item_count++];
    it->label = 0;
    it->sub = NULL;
    it->action = 0;
    it->action_ud = NULL;
    it->dyn_state = NULL;
    it->disabled = true;
}

static void call_item(void *item, void *unused) {
    const menu_item *it = (const menu_item *)item;
    (void)unused;
    if (it->action_ud)
        it->action_ud(it->userdata);
    else if (it->action)
        it->action();
}

/* .APP items through their hook; built-in ones guarded (os.h) */
void menu_item_run(const menu_item *item) {
    wm_invalidate_all();  /* a command can change anything on screen */
    menu_item it = *item; /* the command may rebuild its menu */
    if (it.dyn_state && g_menu_call_hook)
        g_menu_call_hook(it.dyn_state, it.action_ud, it.userdata);
    else if (it.action_ud || it.action)
        os_guard_call(os_instance_of_window(wm_focused_window()), call_item,
                      &it, NULL, "a menu command");
}

void menu_enable_last(menu *m, bool on) {
    if (m->item_count > 0 && m->items[m->item_count - 1].label)
        m->items[m->item_count - 1].disabled = !on;
}

void menubar_close_all(menubar *mb) {
    for (int i = 0; i < mb->menu_count; i++)
        mb->menus[i].open = false;
    mb->open_index = -1;
    mb->sub_item = -1;
}

#define PAGE_BTN_W 14

/* Places the titles: from `first`, while they fit. If some are left over,
 * a >> at the right end shows the next page; on a later page a << after
 * the clock goes back. Menus not on this page get bar_w = 0. */
void menubar_layout(menubar *mb) {
    if (mb->first >= mb->menu_count)
        mb->first = 0, mb->npages = 0;
    int x = 2 + CLOCK_W, right = SCREEN_WIDTH - 2;
    mb->prev_x = mb->next_x = -1;
    if (mb->first > 0) {
        mb->prev_x = x;
        x += PAGE_BTN_W + 2;
    }
    int i;
    for (i = 0; i < mb->first; i++)
        mb->menus[i].bar_x = mb->menus[i].bar_w = 0;
    bool full = false;
    for (; i < mb->menu_count; i++) {
        menu *m = &mb->menus[i];
        int tw = text_width(m->title, UI_FONT_SMALL) + PADDING * 2;
        int need = tw + (i < mb->menu_count - 1 ? PAGE_BTN_W + 4 : 0);
        if (full || (i > mb->first && x + need > right)) {
            full = true; /* this and the rest: the next page */
            m->bar_x = m->bar_w = 0;
            if (mb->next_x < 0)
                mb->next_x = right - PAGE_BTN_W;
            continue;
        }
        m->bar_x = x;
        m->bar_w = tw;
        x += tw + 2;
    }
}

/* the first menu of the next page (after the last one shown) */
static int next_page_start(const menubar *mb) {
    for (int i = mb->menu_count - 1; i >= mb->first; i--)
        if (mb->menus[i].bar_w)
            return i + 1;
    return mb->first + 1;
}

static void draw_page_btn(int x, bool next) {
    fill_rect(x, 1, PAGE_BTN_W, MENUBAR_H - 2, DARK_GRAY);
    /* two small triangles: << or >> */
    for (int t = 0; t < 2; t++) {
        int ax = x + 3 + t * 5, ay = MENUBAR_H / 2;
        for (int k = 0; k < 4; k++) {
            int col = next ? ax + k : ax + 3 - k;
            fill_rect(col, ay - 3 + k, 1, 7 - 2 * k, BAR_FG);
        }
    }
}

static void write_2digit(char *buf, uint8_t val) {
    buf[0] = '0' + (val / 10);
    buf[1] = '0' + (val % 10);
}

/* "HH:MM:SS", or "HH:MM" (Settings > Performance: one redraw a minute) */
void menubar_clock_text(char *out) {
    time_full_t t = time_rtc_local();
    write_2digit(out + 0, t.hours);
    out[2] = ':';
    write_2digit(out + 3, t.minutes);
    out[5] = '\0';
    if (g_cfg.clock_no_secs)
        return;
    out[5] = ':';
    write_2digit(out + 6, t.seconds);
    out[8] = '\0';
}

/* ── dropdowns ─────────────────────────────────────────────────────── */

#define ARROW_W 8 /* room for the submenu arrow */

static int drop_width(const menu *m) {
    int dw = MENU_ITEM_MIN_W;
    bool subs = false;
    for (int j = 0; j < m->item_count; j++) {
        if (!m->items[j].label)
            continue;
        subs |= m->items[j].sub != NULL;
        int iw = text_width(m->items[j].label, UI_FONT_SMALL) + PADDING * 2 + 4;
        if (iw > dw)
            dw = iw;
    }
    return dw + (subs ? ARROW_W : 0);
}

/* where the open menu's submenu goes: right of the item, or left of the
 * dropdown if that runs off the screen */
static bool sub_rect(const menubar *mb, int *x, int *y, int *w, int *h) {
    if (mb->open_index < 0 || mb->sub_item < 0)
        return false;
    const menu *m = &mb->menus[mb->open_index];
    if (mb->sub_item >= m->item_count || !m->items[mb->sub_item].sub)
        return false;
    const menu *sm = m->items[mb->sub_item].sub;
    int dw = drop_width(m);
    *w = drop_width(sm);
    *h = sm->item_count * MENU_ITEM_H;
    if (*h < MENU_ITEM_H)
        *h = MENU_ITEM_H;
    *x = m->bar_x + dw - 2;
    if (*x + *w > SCREEN_WIDTH - 2)
        *x = m->bar_x - *w + 2;
    if (*x < 0)
        *x = 0;
    *y = MENUBAR_H + mb->sub_item * MENU_ITEM_H;
    if (*y + *h > SCREEN_HEIGHT - 2)
        *y = SCREEN_HEIGHT - 2 - *h;
    return true;
}

static void draw_drop(const menu *m, int x, int y, int sel_item) {
    int dw = drop_width(m);
    int dh = m->item_count * MENU_ITEM_H;
    if (!m->item_count) { /* an empty group */
        fill_rect(x + 2, y + 2, dw, MENU_ITEM_H, BLACK);
        fill_rect(x, y, dw, MENU_ITEM_H, DROP_BG);
        draw_rect(x, y, dw, MENU_ITEM_H, BLACK);
        draw_string(x + PADDING, y + 2, "(empty)", DROP_DIS, UI_FONT_SMALL);
        return;
    }
    fill_rect(x + 2, y + 2, dw, dh, BLACK);
    fill_rect(x, y, dw, dh, DROP_BG);
    draw_rect(x, y, dw, dh, BLACK);

    for (int j = 0; j < m->item_count; j++) {
        const menu_item *it = &m->items[j];
        int iy = y + j * MENU_ITEM_H;

        if (!it->label) {
            int sy = iy + MENU_ITEM_H / 2;
            draw_string(x + 2, sy, "---", DROP_SEP, 2);
            continue;
        }

        bool hovered = mouse.x >= x && mouse.x < x + dw && mouse.y >= iy &&
                       mouse.y < iy + MENU_ITEM_H;
        if ((hovered || j == sel_item) && !it->disabled)
            fill_rect(x + 1, iy, dw - 2, MENU_ITEM_H, DROP_SEL);

        unsigned char fg = it->disabled ? DROP_DIS : DROP_FG;
        draw_string(x + PADDING, iy + 2, it->label, fg, UI_FONT_SMALL);
        if (it->sub) { /* a small triangle pointing right */
            int ax = x + dw - ARROW_W, ay = iy + MENU_ITEM_H / 2;
            for (int k = 0; k < 4; k++)
                fill_rect(ax + k, ay - 3 + k, 1, 7 - 2 * k, fg);
        }
    }
}

void menubar_draw(menubar *mb) {
    fill_rect(0, 0, SCREEN_WIDTH, MENUBAR_H, BAR_BG);

    int cursor_x = 2;

    char clock_str[9];
    menubar_clock_text(clock_str);
    draw_string(cursor_x, 2, clock_str, BAR_FG, 2);
    cursor_x += CLOCK_W;

    (void)cursor_x;
    menubar_layout(mb);
    if (mb->prev_x >= 0)
        draw_page_btn(mb->prev_x, false);
    if (mb->next_x >= 0)
        draw_page_btn(mb->next_x, true);
    for (int i = 0; i < mb->menu_count; i++) {
        menu *m = &mb->menus[i];
        if (!m->bar_w)
            continue; /* on another page */
        if (m->open)
            fill_rect(m->bar_x, 0, m->bar_w, MENUBAR_H, DROP_SEL);
        draw_string(m->bar_x + PADDING, 2, m->title, BAR_FG, UI_FONT_SMALL);
    }

    /* the open dropdown and its submenu, over everything */
    if (mb->open_index >= 0) {
        menu *m = &mb->menus[mb->open_index];
        draw_drop(m, m->bar_x, MENUBAR_H, mb->sub_item);
        int sx, sy, sw, sh;
        if (sub_rect(mb, &sx, &sy, &sw, &sh))
            draw_drop(m->items[mb->sub_item].sub, sx, sy, -1);
    }
}

void menubar_update(menubar *mb) {
    menubar_layout(mb);
    if (mouse.left_clicked && mouse.y < MENUBAR_H) {
        g_menubar_click_consumed = true;
        mouse.left_clicked = false;

        if (mouse.x < 2 + CLOCK_W) {
            os_launch_app(&clock_app);
            return;
        }
        if (mb->next_x >= 0 && mouse.x >= mb->next_x &&
            mouse.x < mb->next_x + PAGE_BTN_W) { /* >> */
            menubar_close_all(mb);
            if (mb->npages < MAX_MENUS)
                mb->pages[mb->npages++] = mb->first;
            mb->first = next_page_start(mb);
            menubar_layout(mb);
            return;
        }
        if (mb->prev_x >= 0 && mouse.x >= mb->prev_x &&
            mouse.x < mb->prev_x + PAGE_BTN_W) { /* << */
            menubar_close_all(mb);
            mb->first = mb->npages ? mb->pages[--mb->npages] : 0;
            menubar_layout(mb);
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

    /* the submenu: open while the mouse is on its item or inside it */
    if (mb->open_index >= 0) {
        menu *m = &mb->menus[mb->open_index];
        int dw = drop_width(m), dy = MENUBAR_H;
        int sx, sy, sw, sh;
        bool in_sub = sub_rect(mb, &sx, &sy, &sw, &sh) && mouse.x >= sx &&
                      mouse.x < sx + sw && mouse.y >= sy && mouse.y < sy + sh;
        if (!in_sub && mouse.x >= m->bar_x && mouse.x < m->bar_x + dw &&
            mouse.y >= dy && mouse.y < dy + m->item_count * MENU_ITEM_H) {
            int idx = (mouse.y - dy) / MENU_ITEM_H;
            const menu_item *it = &m->items[idx];
            int want = it->sub && !it->disabled ? idx : -1;
            /* With a submenu showing, another row takes over only once the
             * mouse rests on it: on the way across to the submenu it may
             * brush the row below (as Windows does). */
            static int s_pending = -2;
            static uint32_t s_since;
            if (want == mb->sub_item) {
                s_pending = -2;
            } else if (mb->sub_item < 0) {
                mb->sub_item = want; /* nothing open: at once */
            } else if (want != s_pending) {
                s_pending = want;
                s_since = pit_ticks;
            } else if (pit_ticks - s_since >= 250) {
                mb->sub_item = want;
                s_pending = -2;
            }
        }
        if (in_sub && mouse.left_clicked) {
            g_menubar_click_consumed = true;
            mouse.left_clicked = false;
            const menu *sm = m->items[mb->sub_item].sub;
            int idx = (mouse.y - sy) / MENU_ITEM_H;
            if (idx >= 0 && idx < sm->item_count) {
                const menu_item *it = &sm->items[idx];
                if (it->label && !it->disabled && !it->sub)
                    menu_item_run(it);
            }
            menubar_close_all(mb);
            return;
        }
    }

    if (mb->open_index >= 0 && mouse.left_clicked) {
        menu *m = &mb->menus[mb->open_index];
        int dy = MENUBAR_H;
        int dw = drop_width(m);

        if (mouse.x >= m->bar_x && mouse.x < m->bar_x + dw && mouse.y >= dy &&
            mouse.y < dy + m->item_count * MENU_ITEM_H) {
            g_menubar_click_consumed = true;
            mouse.left_clicked = false;

            int idx = (mouse.y - dy) / MENU_ITEM_H;
            if (idx >= 0 && idx < m->item_count) {
                menu_item *it = &m->items[idx];
                if (it->sub) { /* a group: it stays open */
                    mb->sub_item = it->disabled ? -1 : idx;
                    return;
                }
                if (it->label && !it->disabled)
                    menu_item_run(it);
            }
            menubar_close_all(mb);
        } else {
            g_menubar_click_consumed = true;
            mouse.left_clicked = false;
            menubar_close_all(mb);
        }
        return;
    }

    if (mb->open_index >= 0 && mouse.y < MENUBAR_H) {
        for (int i = 0; i < mb->menu_count; i++) {
            menu *m = &mb->menus[i];
            if (i == mb->open_index)
                continue;
            if (mouse.x >= m->bar_x && mouse.x < m->bar_x + m->bar_w) {
                menubar_close_all(mb);
                m->open = true;
                mb->open_index = i;
                return;
            }
        }
    }
}
