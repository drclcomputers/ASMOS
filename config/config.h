#ifndef CONFIG_H
#define CONFIG_H

#include "lib/types.h"

#define SCREEN_WIDTH    320
#define SCREEN_HEIGHT   200

#define BACKBUF         ((uint8_t*)0x100000)

#define MENUBAR_H       10
#define MENUBAR_H_SIZE  MENUBAR_H
#define TASKBAR_H       10

static bool START_IN_GUI = true;

#endif
