#ifndef UI_CTL_INTERNAL_H
#define UI_CTL_INTERNAL_H
/* Shared between controls.c, ctl_edit.c and dialogs.c. Not for apps. */
#include "ui/controls.h"

void ctl_layout(control *c);
bool ctl_shown(const control *c);
bool ctl_text_reserve(control *c, int need);

void clip_push(int x, int y, int w, int h);
void clip_pop(void);
void th_raised(int x, int y, int w, int h, uint8_t face);
void th_pressed(int x, int y, int w, int h, uint8_t face);
void th_sunken(int x, int y, int w, int h, uint8_t bg);
void th_focus(int x, int y, int w, int h);
void sb_draw(int x, int y, int len, bool vert, int value, int max, int page);
int sb_mouse(int along, int len, int value, int max, int page, bool down,
             int *part);

void edit_draw(control *c);
bool edit_key(control *c);
void edit_mouse(control *c, int mx, int my, bool down, bool dbl);
void edit_fix_scroll(control *c);

void tree_draw(control *c);
void tree_mouse(control *c, int mx, int my, bool down, bool dbl);
bool tree_key(control *c, uint8_t sc, char ch);
void tree_free(control *c);
void tree_select_at(control *c, int mx, int my); /* right-click */
void tree_sel_point(const control *c, int *mx, int *my);

#endif
