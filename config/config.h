#ifndef CONFIG_H
#define CONFIG_H

#define VER "1.1.3"

extern int g_screen_width;
extern int g_screen_height;
extern int g_backbuf_size;
extern int g_video_mode;
#define SCREEN_WIDTH  g_screen_width
#define SCREEN_HEIGHT g_screen_height
#define COLOR_BITS 8
#define RESMODE g_video_mode

#define TARGET_FPS 60
#define FRAME_TIME_MS (1000 / TARGET_FPS)
#define DOUBLE_CLICK_SPEED 500
#define WIN_ANIM_DEFAULT_FRAMES (TARGET_FPS / 3)

#define BACKBUF ((uint8_t *)0x100000)
#define BACKBUF_SIZE g_backbuf_size
#define HEAP_MIN_START (0x100000 + BACKBUF_SIZE)
#define HEAP_END_MAX 0x3F0000

#define BLACK 0
#define BLUE 1
#define GREEN 2
#define CYAN 3
#define RED 4
#define MAGENTA 5
#define BROWN 6
#define LIGHT_GRAY 7
#define DARK_GRAY 8
#define LIGHT_BLUE 9
#define LIGHT_GREEN 10
#define LIGHT_CYAN 11
#define LIGHT_RED 12
#define LIGHT_MAGENTA 13
#define YELLOW 14
#define WHITE 15

#define WALLPAPER_SOLID 0
#define WALLPAPER_CHECKERBOARD 1
#define WALLPAPER_STRIPES 2
#define WALLPAPER_VERTICAL_STRIPES 3
#define WALLPAPER_DOTS 4

#define MENUBAR_H 10
#define MENUBAR_H_SIZE MENUBAR_H
#define TASKBAR_H 10

#endif
