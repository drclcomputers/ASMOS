// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef VIDEO_CFG_H
#define VIDEO_CFG_H

/* Stage 2 reads the mode byte after "VM" at offset 2; changes apply at boot.
 * A mode is a size and a colour count:
 *   0 320x200/256  1 640x400/16   2 640x480/16   3 800x600/16
 *   4 320x200/16   5 640x400/256  6 640x480/256  7 800x600/256
 * (0-3 as before, so old disks keep theirs). Limits: under 3 MB 320x200
 * only, a 386 up to 640x480, 800x600 needs a 486 and 4 MB; 256 colours
 * above 320x200 need the card's VESA BIOS to have the mode, and 800x600
 * the card's mode too. The loader works out which are allowed. */

#include "lib/core.h"

#define VIDEO_320 0
#define VIDEO_640 1
#define VIDEO_640x480 2
#define VIDEO_800 3
#define VIDEO_320_16 4
#define VIDEO_640_256 5
#define VIDEO_640x480_256 6
#define VIDEO_800_256 7
#define VIDEO_MODES 8

#define VIDEO_SIZES 4 /* 320x200, 640x400, 640x480, 800x600 */

int video_mode_size(int mode);           /* 0..3: the size index */
int video_mode_colors(int mode);         /* 16 or 256 */
int video_mode_of(int size, int colors); /* the mode, -1 if none */
int video_size_w(int size);
int video_size_h(int size);
const char *video_size_name(int size); /* "640 x 480" */

void video_cfg_init(uint8_t bios_boot_drive); /* kmain: 0x00.. FDD, 0x80 HDD */
/* the mode stored on the boot disk; -1 if it can't be read (old loader) */
int video_cfg_get(void);
/* store it; NULL or the reason it failed */
const char *video_cfg_set(int mode);

/* what the boot loader found (kernel.c reads it at boot) */
void video_limits_from_boot(void);
bool video_mode_allowed(int mode);
const char *video_mode_name(int mode); /* "640 x 480, 16 colours" */
/* why a mode isn't allowed here; NULL = it is */
const char *video_mode_reason(int mode);

/* the VESA mode in use (256 colours above 320x200), from the loader */
typedef struct {
    uint16_t mode;     /* VBE mode number; 0 = none */
    uint16_t pitch;    /* bytes a line */
    uint32_t lfb;      /* linear frame buffer (physical); 0 = banked */
    uint32_t gran;     /* banked: window positions in steps of this */
    uint32_t win_size; /* bytes of the window */
    uint32_t win_addr; /* where the window is (0xA0000) */
    uint8_t win;       /* 0 = window A, 1 = B */
} vbe_info_t;
extern vbe_info_t g_vbe;

/* the raw values: the allowed modes (bit = mode), the loader's flags, the
 * sizes with a 256-colour VESA mode (bit = size) */
extern uint8_t g_video_allowed, g_video_flags, g_video_256;
#define VF_486 0x01   /* a 486 or better */
#define VF_NO800 0x02 /* the card refused 800x600x16 */
#define VF_VBE 0x04   /* a VESA BIOS */
#define VF_VBE2 0x08  /* VBE 2.0 or later */

#endif
