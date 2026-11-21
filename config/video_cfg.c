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
    return b[VM_OFF + 2] < VIDEO_MODES ? b[VM_OFF + 2] : VIDEO_640;
}

const char *video_cfg_set(int mode) {
    static uint8_t b[512];
    if (mode < 0 || mode >= VIDEO_MODES)
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

// what this machine may use (boot/stage2.asm step 5)

#define VMAX_486 0x40
#define VMAX_NO800 0x80

uint8_t g_video_limits = VIDEO_800; /* (no boot info: everything) */

int video_max_mode(void) { return g_video_limits & 3; }

const char *video_mode_name(int mode) {
    switch (mode) {
    case VIDEO_320:
        return "320 x 200, 256 colours";
    case VIDEO_640:
        return "640 x 400, 16 colours";
    case VIDEO_640x480:
        return "640 x 480, 16 colours";
    case VIDEO_800:
        return "800 x 600, 16 colours";
    }
    return "?";
}

const char *video_limit_reason(void) {
    if (video_max_mode() == VIDEO_800)
        return NULL;
    if (g_video_limits & VMAX_NO800)
        return "the video card has no 800 x 600 mode";
    if (video_max_mode() == VIDEO_320)
        return "bigger screens need 3 MB of RAM";
    if (!(g_video_limits & VMAX_486))
        return "800 x 600 needs a 486 or better";
    return "800 x 600 needs 4 MB of RAM";
}
