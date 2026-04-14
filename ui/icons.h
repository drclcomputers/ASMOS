#ifndef ICONS_H
#define ICONS_H

#include "lib/core.h"

#define ICON_W         20
#define ICON_H         20
#define ICON_LABEL_MAX 16
#define ICON_SPACING_X 36
#define ICON_SPACING_Y 34
#define ICON_START_X   8
#define ICON_START_Y   14
#define ICON_LABEL_DISPLAY_MAX 8

#define MAX_ICONS_PER_VIEW 32
#define DBLCLICK_TICKS 60
#define DRAG_THRESHOLD 4

typedef void (*icon_action_t)(void);

typedef struct {
    int           x, y;
    char          label[ICON_LABEL_MAX];
    icon_action_t on_launch;
    bool          selected;
    bool          used;
    void        (*on_draw)(int abs_x, int abs_y);

    bool          dragging;
    int           drag_off_x, drag_off_y;
} icon_t;

typedef struct {
    icon_t   icons[MAX_ICONS_PER_VIEW];
    int      icon_count;

    int      origin_x, origin_y;
    int      area_w,   area_h;

    int      _last_click_idx;
    uint32_t _last_click_tick;

    int      _drag_idx;
} icon_view_t;

#define ICO_W         20
#define ICO_H         20
#define ICO_LABEL_W    5
#define ICO_LABEL_H    6
#define ICO_CELL_H    34
#define ICO_LABEL_MAX  8
#define ICON_SZ_W       20
#define ICON_SZ_H       20

void draw_file_icon(int ax, int ay, bool sel);
void draw_folder_icon(int ax, int ay, bool sel);
void draw_app_icon(int ax, int ay, bool sel);
void draw_dotdot_icon(int ax, int ay, bool sel);
void draw_pic_icon(int ax, int ay, bool sel);
void draw_txt_icon(int ax, int ay, bool sel);
void draw_cfg_icon(int ax, int ay, bool sel);
void draw_unknown_icon(int ax, int ay, bool sel);

void icon_view_init(icon_view_t *v, int origin_x, int origin_y, int area_w, int area_h);

void icon_view_set_origin(icon_view_t *v, int origin_x, int origin_y, int area_w, int area_h);

int  icon_view_add(icon_view_t *v, const char *label, icon_action_t on_launch, int x, int y);

void icon_view_remove(icon_view_t *v, int idx);

icon_t *icon_view_get(icon_view_t *v, int *count_out);

void icon_view_update(icon_view_t *v, bool blocked);

void icon_view_draw(const icon_view_t *v);


void desktop_icons_init(int origin_x, int origin_y, int area_w, int area_h);
int desktop_icon_add(const char *label, icon_action_t on_launch, int x, int y);
void desktop_icon_remove(int idx);
icon_t *desktop_icons_get(int *count_out);
void desktop_icons_update(bool window_captured);
void desktop_icons_draw(void);

#endif
