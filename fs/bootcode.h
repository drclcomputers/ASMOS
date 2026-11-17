#ifndef FS_BOOTCODE_H
#define FS_BOOTCODE_H
/* The boot code (boot/bootcode.asm): the master boot record, the volume
 * boot record (its BPB a template: 0x0B..0x3D are the volume's) and stage 2
 * (whole sectors; byte 4 of the first is the screen mode). */
#include "lib/core.h"
extern const uint8_t boot_mbr[512];
extern const uint8_t boot_vbr[512];
extern const uint8_t boot_stage2[];
extern const uint8_t boot_stage2_end[];
#define BOOT_STAGE2_SECTORS                                                    \
    ((uint32_t)(boot_stage2_end - boot_stage2) / 512u)
#endif
