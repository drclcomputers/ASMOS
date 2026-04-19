#ifndef CONFIG_H
#define CONFIG_H

#define VER "0.6.7"

// Memory
#define BACKBUF ((uint8_t *)0x100000)
#define HEAP_START 0x164000
#define HEAP_END 0x464000

// Screen/Display
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200
#define TARGET_FPS 60
#define FRAME_TIME_MS (1000 / TARGET_FPS)

#define BLACK 0
#define BLUE 1
#define GREEN 2
#define YELLOW 3
#define RED 4
#define MAGENTA 5
#define CYAN 6
#define LIGHT_GRAY 7
#define DARK_GRAY 8
#define LIGHT_BLUE 9
#define LIGHT_GREEN 10
#define LIGHT_YELLOW 11
#define LIGHT_RED 12
#define LIGHT_MAGENTA 13
#define LIGHT_CYAN 14
#define WHITE 15

#define MENUBAR_H 10
#define MENUBAR_H_SIZE MENUBAR_H
#define TASKBAR_H 10

#endif
