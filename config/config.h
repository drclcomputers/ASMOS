#ifndef CONFIG_H
#define CONFIG_H

#define VER "0.9.9"

// Memory
#define BACKBUF ((uint8_t *)0x100000)
#define HEAP_START 0x200000
#define HEAP_END 0x464000

// Screen/Display
#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 400
//#define SCREEN_HEIGHT 480
#define VESA_MODE 0x0100 // 640x400x8
//#define VESA_MODE 0x0101 // 640x480x8
#define VESA_FB 0xE0000000
#define TARGET_FPS 60
#define FRAME_TIME_MS (1000 / TARGET_FPS)
#define WIN_ANIM_DEFAULT_FRAMES TARGET_FPS / 3

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
#define WALLPAPER_DOTS 3

#define MENUBAR_H 10
#define MENUBAR_H_SIZE MENUBAR_H
#define TASKBAR_H 10

#endif
