// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef FS_INSTALL_H
#define FS_INSTALL_H

#include "lib/core.h"

typedef enum {
    INST_NONE,  /* no disk */
    INST_EMPTY, /* nothing recognisable: new, or wiped */
    INST_ASMOS, /* an ASMOS volume */
    INST_FAT,   /* some other FAT12/16 volume (DOS, Windows...) */
    INST_OTHER, /* partitions, but none FAT12/16 (another OS, FAT32...) */
} inst_kind_t;

typedef struct {
    inst_kind_t kind;
    char model[41];
    uint32_t sectors; /* the disk's */
    bool partitioned; /* an MBR (else one volume from sector 0) */
    uint32_t vol_lba; /* the volume, if there is one */
    uint32_t vol_sectors;
    char label[12];
    bool can_keep;        /* install over it, keeping its files */
    const char *why_not;  /* when not: why */
    uint32_t new_sectors; /* the volume inst_erase would make */
} inst_disk_t;

typedef void (*inst_progress_fn)(void *user, int pct, const char *what);

void inst_probe(uint8_t unit, inst_disk_t *d);
const char *inst_erase(uint8_t unit, inst_progress_fn cb, void *user);
/* the same, with a volume label (NULL or "": ASMOS's own, "ASMOS") */
const char *inst_erase_label(uint8_t unit, const char *label,
                             inst_progress_fn cb, void *user);
/* video_mode: config/video_cfg.h (0 = 320x200 .. 3 = 800x600), -1 = what
 * the disk had (else 1) */
const char *inst_write_boot(uint8_t unit, int video_mode);
/* zip: an ASMOS.ZIP to extract into the drive's root (NULL: none), with
 * SYSTEM/KERNEL.SYS in it compressed; kernel: a KERNEL.SYS to copy if the
 * zip has none (NULL: none); replace: overwrite files already there */
const char *inst_copy_files(uint8_t drive, const char *kernel, const char *zip,
                            bool replace, inst_progress_fn cb, void *user);

/* where a setup floppy keeps them */
#define SETUP_KERNEL "/FDD0/SYSTEM/KERNEL.SYS"
#define SETUP_ZIP "/FDD0/SETUP/ASMOS.ZIP"

#endif
