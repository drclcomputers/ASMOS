#ifndef FS_FDFORMAT_H
#define FS_FDFORMAT_H

#include "lib/core.h"

/*
 * Creates a DOS-compatible floppy layout; quick mode skips track formatting.
 * fdformat_begin() starts the job;
 * fdformat_step() formats one track per call, returns progress, and reports
 * errors through f.err. The drive is remounted when complete.
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
