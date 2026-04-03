#ifndef WIDGETS_H
#define WIDGETS_H

#include "lib/types.h"
#include "lib/string.h"

#define MAX_MENU_ITEMS 12
#define MAX_DROP_ITEMS 8

typedef struct widget widget;
typedef bool (*WidgetCallback)(widget *wg);

typedef enum {
    WIDGET_BUTTON,
    WIDGET_LABEL,
    WIDGET_CHECKBOX,
    WIDGET_TEXTBOX,
    WIDGET_DROPDOWN,
    WIDGET_MENU,
    WIDGET_SCROLLBAR_VERTICAL,
    WIDGET_SCROLLBAR_HORIZONTAL,
} widget_type;

typedef struct {
    char           *label;
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
    char buf[1024];
    int  len;
    bool focused;
} widget_textbox;

typedef struct {
    char          *items[MAX_DROP_ITEMS];
    int            item_count;
    int            selected;
    bool           open;
    WidgetCallback on_select;
} widget_dropdown;

typedef struct {
    char          *items[MAX_MENU_ITEMS];
    WidgetCallback callbacks[MAX_MENU_ITEMS];
    int            item_count;
    bool           open;
} widget_menu;

typedef struct {
    int            value;        // Current scroll position (0 to max)
    int            max;          // Maximum scroll value
    int            viewport;     // Visible area size
    bool           dragging;
    int            drag_offset;
    WidgetCallback on_change;
} widget_scrollbar;

struct widget {
    widget_type   type;
    int           x, y, w, h;
    unsigned char bg_color;
    unsigned char fg_color;
    unsigned char border_color;

    union {
        widget_button    button;
        widget_label     label;
        widget_checkbox  checkbox;
        widget_textbox   textbox;
        widget_dropdown  dropdown;
        widget_menu      menu;
        widget_scrollbar scrollbar;
    } as;
};

static inline widget make_button(int x, int y, int w, int h,
                                  char *label,
                                  unsigned char bg, unsigned char fg,
                                  unsigned char border,
                                  WidgetCallback on_click) {
    widget wg = {0};
    wg.type = WIDGET_BUTTON;
    wg.x = x; wg.y = y; wg.w = w; wg.h = h;
    wg.bg_color = bg; wg.fg_color = fg; wg.border_color = border;
    wg.as.button.label    = label;
    wg.as.button.on_click = on_click;
    return wg;
}

static inline widget make_label(int x, int y, char *text,
                                 unsigned char color, int font_size) {
    widget wg = {0};
    wg.type = WIDGET_LABEL;
    wg.x = x; wg.y = y;
    wg.fg_color           = color;
    wg.as.label.text      = text;
    wg.as.label.color     = color;
    wg.as.label.font_size = font_size;
    return wg;
}

static inline widget make_checkbox(int x, int y, char *label,
                                    unsigned char bg, unsigned char fg, unsigned char border,
                                    bool checked, WidgetCallback on_change) {
    widget wg = {0};
    wg.type = WIDGET_CHECKBOX;
    wg.x = x; wg.y = y; wg.w = 10; wg.h = 10;
    wg.bg_color = bg; wg.fg_color = fg; wg.border_color = border;
    wg.as.checkbox.label     = label;
    wg.as.checkbox.checked   = checked;
    wg.as.checkbox.on_change = on_change;
    return wg;
}

static inline widget make_textbox(int x, int y, int w, int h, unsigned char bg, unsigned char fg, unsigned char border) {
    widget wg = {0};
    wg.type = WIDGET_TEXTBOX;
    wg.x = x; wg.y = y; wg.w = w; wg.h = h;
    wg.bg_color = bg; wg.fg_color = fg; wg.border_color = border;
    return wg;
}

static inline widget make_dropdown(int x, int y, int w, int h,
                                    unsigned char bg, unsigned char fg,
                                    unsigned char border,
                                    char **items, int count,
                                    WidgetCallback on_select) {
    widget wg = {0};
    wg.type = WIDGET_DROPDOWN;
    wg.x = x; wg.y = y; wg.w = w; wg.h = h;
    wg.bg_color = bg; wg.fg_color = fg; wg.border_color = border;
    int n = count < MAX_DROP_ITEMS ? count : MAX_DROP_ITEMS;
    wg.as.dropdown.item_count = n;
    wg.as.dropdown.selected   = -1;
    wg.as.dropdown.on_select  = on_select;
    for (int i = 0; i < n; i++)
        wg.as.dropdown.items[i] = items[i];
    return wg;
}

static inline widget make_vscrollbar(int x, int y, int h,
                                     unsigned char bg, unsigned char fg, unsigned char border,
                                     int max, int viewport, WidgetCallback on_change) {
    widget wg = {0};
    wg.type = WIDGET_SCROLLBAR_VERTICAL;
    wg.x = x; wg.y = y; wg.w = 12; wg.h = h;
    wg.bg_color = bg; wg.fg_color = fg; wg.border_color = border;
    wg.as.scrollbar.value     = 0;
    wg.as.scrollbar.max       = max;
    wg.as.scrollbar.viewport  = viewport;
    wg.as.scrollbar.dragging  = false;
    wg.as.scrollbar.on_change = on_change;
    return wg;
}

static inline widget make_hscrollbar(int x, int y, int w,
                                     unsigned char bg, unsigned char fg, unsigned char border,
                                     int max, int viewport, WidgetCallback on_change) {
    widget wg = {0};
    wg.type = WIDGET_SCROLLBAR_HORIZONTAL;
    wg.x = x; wg.y = y; wg.w = w; wg.h = 12;
    wg.bg_color = bg; wg.fg_color = fg; wg.border_color = border;
    wg.as.scrollbar.value     = 0;
    wg.as.scrollbar.max       = max;
    wg.as.scrollbar.viewport  = viewport;
    wg.as.scrollbar.dragging  = false;
    wg.as.scrollbar.on_change = on_change;
    return wg;
}

void widget_draw(widget *wg, int win_x, int win_y);
void widget_update(widget *wg, int win_x, int win_y);

#endif
