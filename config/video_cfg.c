#include "config/video_cfg.h"

#include "fs/fs.h"

#define VM_LBA 1 /* first stage-2 sector */
#define VM_OFF 2 /* "VM" at 2-3, the mode at 4 */

static uint8_t s_drive = DRIVE_HDA;

void video_cfg_init(uint8_t bios_boot_drive) {
    s_drive = bios_boot_drive < 0x80 ? DRIVE_FDD0 : DRIVE_HDA;
}

static bool read_block(uint8_t *b) {
    return fs_read_sector(s_drive, VM_LBA, b) && b[VM_OFF] == 'V' &&
           b[VM_OFF + 1] == 'M';
}

int video_cfg_get(void) {
    static uint8_t b[512];
    if (!read_block(b))
        return -1;
    return b[VM_OFF + 2] ? VIDEO_640 : VIDEO_320;
}

const char *video_cfg_set(int mode) {
    static uint8_t b[512];
    if (mode != VIDEO_320 && mode != VIDEO_640)
        return "unknown screen mode";
    if (!read_block(b))
        return "the boot disk has an older loader (rebuild the image)";
    if (b[VM_OFF + 2] == (uint8_t)mode)
        return NULL;
    b[VM_OFF + 2] = (uint8_t)mode;
    if (!fs_write_sector(s_drive, VM_LBA, b))
        return "could not write the boot disk (write-protected?)";
    return video_cfg_get() == mode ? NULL : "the boot disk did not keep it";
}
