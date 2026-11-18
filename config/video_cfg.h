#ifndef VIDEO_CFG_H
#define VIDEO_CFG_H

/*
 * The screen mode is chosen in real mode by the boot loader, before the
 * kernel runs, so it is stored on the boot disk itself: "VM" + one byte at
 * offset 2 of the first stage-2 sector (sector 1 of the boot volume: the
 * partition's on a hard disk, which fs_read_sector adds; boot/stage2.asm).
 *   VIDEO_320     320x200, 256 colours (BIOS mode 13h)
 *   VIDEO_640     640x400, 16 colours (mode 12h reprogrammed to 400 lines)
 *   VIDEO_640x480 640x480, 16 colours (mode 12h)
 *   VIDEO_800     800x600, 16 colours (SVGA mode 6Ah / VESA 102h)
 * A change takes effect at the next boot.
 *
 * The boot loader also limits it to what the machine can take: under 3 MB
 * 320x200 only, a 386 up to 640x480, 800x600 with a 486 and 4 MB (and a
 * video card that has the mode). The numbers grow with the screen, so the
 * allowed ones are always 0 .. video_max_mode().
 */

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
