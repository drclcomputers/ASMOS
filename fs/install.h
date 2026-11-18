#ifndef FS_INSTALL_H
#define FS_INSTALL_H

/*
 * Putting ASMOS on a hard disk (fs/install.c): what Setup (apps/setup.c)
 * and the shell's SYS command do, step by step.
 *
 *   inst_probe        what is on a disk now
 *   inst_erase        a new partition table and an empty FAT16 volume
 *                     (the whole disk, up to 2 GB): everything on it is lost
 *   inst_write_boot   the boot code: MBR (keeping the partition table), the
 *                     volume's boot sector (keeping its BPB) and stage 2 in
 *                     its reserved sectors; the volume becomes the active one
 *   inst_copy_files   /SYSTEM/KERNEL.SYS and what is in ASMOS.ZIP (/APPS...)
 *
 * The disk layout these make, and the boot that follows from it, are in
 * boot/bootdefs.inc and tools/mkdisk.py (which builds the same thing).
 * Units are ATA units: 0 = HDA, 1 = HDB (also their DRIVE_ ids).
 */

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
