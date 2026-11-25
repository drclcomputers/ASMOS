// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "config/video_cfg.h"

#include "fs/fs.h"
#include "lib/string.h"

#define VM_LBA 1 /* first stage-2 sector */
#define VM_OFF 2 /* "VM" at 2-3, the mode at 4 */

// the modes

static const uint8_t k_size[VIDEO_MODES] = {0, 1, 2, 3, 0, 1, 2, 3};
static const uint8_t k_256[VIDEO_MODES] = {1, 0, 0, 0, 0, 1, 1, 1};
static const uint16_t k_w[VIDEO_SIZES] = {320, 640, 640, 800};
static const uint16_t k_h[VIDEO_SIZES] = {200, 400, 480, 600};

int video_mode_size(int m) { return m >= 0 && m < VIDEO_MODES ? k_size[m] : 0; }
int video_mode_colors(int m) {
    return m >= 0 && m < VIDEO_MODES && k_256[m] ? 256 : 16;
}
int video_size_w(int s) { return s >= 0 && s < VIDEO_SIZES ? k_w[s] : 0; }
int video_size_h(int s) { return s >= 0 && s < VIDEO_SIZES ? k_h[s] : 0; }

int video_mode_of(int size, int colors) {
    for (int m = 0; m < VIDEO_MODES; m++)
        if (k_size[m] == size && video_mode_colors(m) == colors)
            return m;
    return -1;
}

const char *video_size_name(int s) {
    static const char *const n[VIDEO_SIZES] = {"320 x 200", "640 x 400",
                                               "640 x 480", "800 x 600"};
    return s >= 0 && s < VIDEO_SIZES ? n[s] : "?";
}

const char *video_mode_name(int m) {
    static char b[VIDEO_MODES][28];
    if (m < 0 || m >= VIDEO_MODES)
        return "?";
    if (!b[m][0])
        snprintf(b[m], sizeof(b[m]), "%s, %d colours",
                 video_size_name(k_size[m]), video_mode_colors(m));
    return b[m];
}

// the boot disk

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

// what this machine may use (boot/stage2.asm step 5, boot/bootdefs.inc)

uint8_t g_video_allowed = 0xFF; /* (no boot info: everything) */
uint8_t g_video_flags = VF_486 | VF_VBE | VF_VBE2, g_video_256 = 0x0E;
vbe_info_t g_vbe;
static uint32_t s_ram = 0; /* 0 = unknown */

#define BI8(a) (*(volatile uint8_t *)(a))
#define BI16(a) (*(volatile uint16_t *)(a))
#define BI32(a) (*(volatile uint32_t *)(a))

void video_limits_from_boot(void) {
    s_ram = BI32(0x0500);
    if (BI8(0x0620) != 'V') { /* an older loader: highest mode + flags */
        uint8_t old = BI8(0x0603);
        g_video_allowed = (uint8_t)((2u << (old & 3)) - 1);
        g_video_flags = (old & 0x40 ? VF_486 : 0) | (old & 0x80 ? VF_NO800 : 0);
        g_video_256 = 0;
        return;
    }
    g_video_allowed = BI8(0x0603) | 1; /* (320x200x256 always works) */
    g_video_flags = BI8(0x0621);
    g_video_256 = BI8(0x0622);
    g_vbe.win = BI8(0x0623);
    g_vbe.mode = BI16(0x0624);
    g_vbe.pitch = BI16(0x0626);
    g_vbe.lfb = BI32(0x0628);
    g_vbe.gran = (uint32_t)BI16(0x062C) * 1024;
    g_vbe.win_size = (uint32_t)BI16(0x062E) * 1024;
    g_vbe.win_addr = (uint32_t)BI16(0x0630) << 4;
}

bool video_mode_allowed(int m) {
    return m >= 0 && m < VIDEO_MODES && (g_video_allowed >> m) & 1;
}

const char *video_mode_reason(int m) {
    if (video_mode_allowed(m))
        return NULL;
    int s = video_mode_size(m);
    if (s > 0 && s_ram && s_ram < 0x2C0000)
        return "bigger screens need 3 MB of RAM";
    if (s == 3 && !(g_video_flags & VF_486))
        return "800 x 600 needs a 486 or better";
    if (s == 3 && s_ram && s_ram < 0x3C0000)
        return "800 x 600 needs 4 MB of RAM";
    if (video_mode_colors(m) == 256 && s > 0)
        return g_video_flags & VF_VBE
                   ? "the video card has no 256-colour mode of that size"
                   : "256 colours that big need a VESA video card";
    if (m == VIDEO_800)
        return "the video card has no 800 x 600 mode";
    return "not on this machine";
}
