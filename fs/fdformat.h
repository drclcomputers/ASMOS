#ifndef FS_FDFORMAT_H
#define FS_FDFORMAT_H

#include "lib/core.h"

/*
 * Formatting floppies the way DOS does: a low-level format of every track
 * (unless "quick"), then a boot sector, two FATs and an empty root
 * directory with the standard layout for the size, so the disk works in
 * real DOS and Windows machines.
 *
 * It runs a track at a time so a window can show progress:
 *   fdformat_t f;
 *   if (fdformat_begin(&f, DRIVE_FDD0, FDD_MEDIA_720K, "WORK", false))
 *       while ((pct = fdformat_step(&f)) >= 0 && pct < 100) ...;
 *   pct < 0: f.err says why. The drive is mounted again at the end.
 */

typedef struct {
    uint8_t drive_id; /* DRIVE_FDD0 / DRIVE_FDD1 */
    uint8_t unit;     /* 0 / 1 */
    int media;        /* FDD_MEDIA_* */
    char label[12];
    bool quick;
    int track, tracks; /* track = cylinder * heads + head */
    bool done;
    const char *err;
} fdformat_t;

bool fdformat_begin(fdformat_t *f, uint8_t drive_id, int media,
                    const char *label, bool quick);
int fdformat_step(fdformat_t *f); /* percent; 100 = done; -1 = failed */

/* The empty FAT12 file system alone, through `wr` (also used to build disk
 * images on the host). serial: the volume serial number. */
typedef bool (*fdformat_write_fn)(uint32_t lba, const void *sector, void *u);
bool fdformat_write_fat(int media, const char *label, uint32_t serial,
                        fdformat_write_fn wr, void *u);

#endif
