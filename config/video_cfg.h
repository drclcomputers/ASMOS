#ifndef VIDEO_CFG_H
#define VIDEO_CFG_H

/*
 * The screen mode is chosen in real mode by the boot loader, before the
 * kernel runs, so it is stored on the boot disk itself: "VM" + one byte at
 * offset 2 of the first stage-2 sector (LBA 1; boot/boot_stage2.asm).
 *   VIDEO_320 = 320x200, 256 colours (BIOS mode 13h)
 *   VIDEO_640 = 640x400, 16 colours (mode 12h reprogrammed to 400 lines)
 * A change takes effect at the next boot.
 */

#include "lib/core.h"

#define VIDEO_320 0
#define VIDEO_640 1

void video_cfg_init(uint8_t bios_boot_drive); /* kmain: 0x00.. FDD, 0x80 HDD */
/* the mode stored on the boot disk; -1 if it can't be read (old loader) */
int video_cfg_get(void);
/* store it; NULL or the reason it failed */
const char *video_cfg_set(int mode);

#endif
