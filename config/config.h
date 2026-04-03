#ifndef CONFIG_H
#define CONFIG_H

#include "config/config_enduser.h"

#define BACKBUF         ((uint8_t*)0x100000)

#define MENUBAR_H         10
#define MENUBAR_H_SIZE    MENUBAR_H
#define TASKBAR_H         10

#define HEAP_START 0x164000
#define HEAP_END   0x400000

#endif
