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
#define SETUP_INF "/FDD0/SETUP/SETUP.INF"

// the apps Setup lets the user choose (SETUP.INF, from extapps/MANIFEST)

enum { PART_MIN, PART_MEDIUM, PART_FULL };           /* the smallest install */
enum { PART_ANY, PART_MEDIA, PART_GAMES, PART_NET }; /* asked about */

#define INST_PARTS_MAX 32

typedef struct {
    char app[16];  /* its source name: "picview" */
    char name[32]; /* "Picture Viewer" */
    char path[80]; /* in APPS: "Accessories/Picture Viewer.APP" */
    uint8_t size, group;
    uint16_t kb;
} inst_part_t;

/* SETUP.INF's apps into parts; how many (0: no such file) */
int inst_read_parts(const char *inf, inst_part_t *parts, int max);
/* the app is in the drive's APPS */
bool inst_part_installed(uint8_t drive, const inst_part_t *p);
/* inst_copy_files, but of the apps only those want[k] says (the kernel,
 * the shortcuts, everything else: all of it) */
const char *inst_copy_files_parts(uint8_t drive, const char *kernel,
                                  const char *zip, const inst_part_t *parts,
                                  int n, const bool *want, inst_progress_fn cb,
                                  void *user);
/* an installed ASMOS: the apps want[k] says copied in (from zip), the
 * others taken out (a group's folder too, once empty). Nothing else is
 * touched. */
const char *inst_change_parts(uint8_t drive, const char *zip,
                              const inst_part_t *parts, int n, const bool *want,
                              inst_progress_fn cb, void *user);

// settings Setup asks about, into the drive's ASMOS.CFG

typedef struct {
    bool network, sound, chime;
    uint8_t printer_type, printer_port;
} inst_settings_t;

/* kept: what the drive's ASMOS.CFG has (else the defaults), these
 * changed */
const char *inst_write_settings(uint8_t drive, const inst_settings_t *set);
/* the drive's ASMOS.CFG: networking on (else the default); and only that
 * changed */
bool inst_read_network(uint8_t drive);
const char *inst_write_network(uint8_t drive, bool on);

#endif
