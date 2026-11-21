#ifndef VIDEO_CFG_H
#define VIDEO_CFG_H

/* Stage 2 reads the mode byte after "VM" at offset 2; changes apply at boot.
 * VIDEO_320=320x200/256, VIDEO_640=640x400/16, VIDEO_640x480=640x480/16,
 * VIDEO_800=800x600/16. Choices depend on CPU, memory, and video hardware. */

#include "lib/core.h"

#define VIDEO_320 0
#define VIDEO_640 1
#define VIDEO_640x480 2
#define VIDEO_800 3
#define VIDEO_MODES 4

void video_cfg_init(uint8_t bios_boot_drive); /* kmain: 0x00.. FDD, 0x80 HDD */
/* the mode stored on the boot disk; -1 if it can't be read (old loader) */
int video_cfg_get(void);
/* store it; NULL or the reason it failed */
const char *video_cfg_set(int mode);

/* what the boot loader found (0x0603, kernel.c keeps it) */
extern uint8_t g_video_limits;
int video_max_mode(void); /* the highest mode this machine may use */
const char *video_mode_name(int mode); /* "640 x 480, 16 colours" */
/* why the modes above video_max_mode() aren't offered; NULL = all are */
const char *video_limit_reason(void);

#endif
