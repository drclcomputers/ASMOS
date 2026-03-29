#ifndef WIDGETS_H
#define WIDGETS_H

#include "lib/types.h"
#include "lib/string.h"

#define MAX_MENU_ITEMS  8
#define MAX_DROP_ITEMS  8

typedef struct widget widget;

typedef bool (*WidgetCallback)(widget *wg);

typedef enum {
    WIDGET_BUTTON,
    WIDGET_LABEL,
    WIDGET_CHECKBOX,
    WIDGET_TEXTBOX,
    WIDGET_DROPDOWN,
    WIDGET_MENU,
} widget_type;

typedef struct {
    char            *label;
    WidgetCallback  on_click;
} widget_button;

typedef struct {
    char          *text;
    unsigned char  color;
    int            font_size;   // 1 = large (8x8), 2 = small (4x6)
} widget_label;

typedef struct {
    char          *label;
    bool           checked;
    WidgetCallback on_change;
} widget_checkbox;

typedef struct {
    char buf[64];
    int  len;
    bool focused;
} widget_textbox;

typedef struct {
    char *items[MAX_DROP_ITEMS];
    int   item_count;
    int   selected;             // -1 - nothing selected
    bool  open;
    WidgetCallback on_select;
} widget_dropdown;

typedef struct {
    char          *items[MAX_MENU_ITEMS];
    WidgetCallback callbacks[MAX_MENU_ITEMS];
    int            item_count;
    bool           open;
} widget_menu;

struct widget {
    widget_type type;
    int x, y;
    int w, h;
    unsigned char bg_color, fg_color, border_color;

    union {
        widget_button   button;
        widget_label    label;
        widget_checkbox checkbox;
        widget_textbox  textbox;
        widget_dropdown dropdown;
        widget_menu     menu;
    } as;
};

static inline widget make_button(int x, int y, int w, int h, unsigned char bg_color, unsigned char fg_color, unsigned char border_color, char *label, WidgetCallback on_click) {
    widget wg = {0};
    wg.type = WIDGET_BUTTON;
    wg.x = x; wg.y = y; wg.w = w; wg.h = h;
    wg.as.button.label    = label;
    wg.as.button.on_click = on_click;
    return wg;
}

static inline widget make_label(int x, int y, char *text, unsigned char bg_color, unsigned char fg_color, unsigned char border_color, int font_size) {
    widget wg = {0};
    wg.type = WIDGET_LABEL;
    wg.x = x; wg.y = y;
    wg.w = 0; wg.h = 0;
    wg.as.label.text      = text;
    wg.as.label.color     = color;
    wg.as.label.font_size = font_size;
    return wg;
}

static inline widget make_checkbox(int x, int y, char *label, unsigned char bg_color, unsigned char fg_color, unsigned char border_color, bool checked, WidgetCallback on_change) {
    widget wg = {0};
    wg.type = WIDGET_CHECKBOX;
    wg.x = x; wg.y = y; wg.w = 10; wg.h = 10;
    wg.as.checkbox.label     = label;
    wg.as.checkbox.checked   = checked;
    wg.as.checkbox.on_change = on_change;
    return wg;
}

static inline widget make_textbox(int x, int y, int w, int h, unsigned char bg_color, unsigned char fg_color, unsigned char border_color) {
    widget wg = {0};
    wg.type = WIDGET_TEXTBOX;
    wg.x = x; wg.y = y; wg.w = w; wg.h = h;
    return wg;
}

static inline widget make_dropdown(int x, int y, int w, int h,
unsigned char bg_color, unsigned char fg_color, unsigned char border_color, char **items, int count, WidgetCallback on_select) {
    widget wg = {0};
    wg.type = WIDGET_DROPDOWN;
    wg.x = x; wg.y = y; wg.w = w; wg.h = h;
    wg.as.dropdown.item_count = count < MAX_DROP_ITEMS ? count : MAX_DROP_ITEMS;
    wg.as.dropdown.selected   = -1;
    wg.as.dropdown.on_select  = on_select;
    for (int i = 0; i < wg.as.dropdown.item_count; i++)
        wg.as.dropdown.items[i] = items[i];
    return wg;
}

void widget_draw(widget *wg, int win_x, int win_y);
void widget_update(widget *wg, int win_x, int win_y);

#endif
