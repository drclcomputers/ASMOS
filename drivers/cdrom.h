#ifndef CDROM_H
#define CDROM_H

#include "lib/core.h"

/*
 * The CD-ROM drive: an ATAPI device on the IDE cables (drivers/ata.c finds
 * it with the hard disks). Read only, PIO, polled. The first CD drive found
 * is the one used. Sectors are 2048 bytes.
 *
 * The file system on it is fs/iso9660.c; it shows as /CD0 (fs/fs.c).
 */

#define CD_SECTOR 2048

typedef enum {
    CD_NONE,     /* no drive */
    CD_EMPTY,    /* no disc (or the tray is open) */
    CD_SPINNING, /* a disc, not ready yet */
    CD_CHANGED,  /* a disc was put in (or swapped) since the last look */
    CD_READY,
} cd_state_t;

bool cd_present(void);
const char *cd_model(void);
cd_state_t cd_check(void); /* TEST UNIT READY (+ REQUEST SENSE) */
/* count sectors (1..16) from lba; false on a read error */
bool cd_read(uint32_t lba, uint32_t count, void *buf);
bool cd_capacity(uint32_t *sectors);
bool cd_eject(void); /* opens the tray (where it has one) */

#endif
