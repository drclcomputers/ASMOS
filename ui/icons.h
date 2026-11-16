#ifndef ICONS_H
#define ICONS_H

#include "lib/core.h"

/* Icon bitmap size (shared by desktop and FileF). */
#define ICON_W 20
#define ICON_H 20
/* Desktop auto-layout grid (ui/desktop_fs.c). */
#define ICON_SPACING_X 36
#define ICON_SPACING_Y 34
#define ICON_START_X 8
#define ICON_START_Y 14
/* Desktop label metrics. */
#define ICO_LABEL_W 5
#define ICO_LABEL_H 6
#define ICO_LABEL_MAX 8

void draw_file_icon(int ax, int ay, bool sel);
void draw_folder_icon(int ax, int ay, bool sel);
void draw_hdd_icon(int ax, int ay, bool sel);
void draw_floppy_icon(int ax, int ay, bool sel);
void draw_app_icon(int ax, int ay, bool sel);
void draw_dotdot_icon(int ax, int ay, bool sel);
void draw_pic_icon(int ax, int ay, bool sel);
void draw_txt_icon(int ax, int ay, bool sel);
void draw_cfg_icon(int ax, int ay, bool sel);
void draw_zip_icon(int ax, int ay, bool sel);
void draw_unknown_icon(int ax, int ay, bool sel);
void draw_trash_icon(int ax, int ay, bool sel, bool full);

#endif
