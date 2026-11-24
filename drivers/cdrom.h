// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef CDROM_H
#define CDROM_H

#include "lib/core.h"

/* Read-only, polled PIO for the first ATAPI CD-ROM found by drivers/ata.c.
 * Sectors are 2048 bytes; fs/iso9660.c exposes it as /CD0. */

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

// Audio CD
/* The drive plays tracks itself through its analogue output (to the sound
 * card's CD input or its headphone socket). */

#define CD_MAX_TRACKS 99
#define CD_FPS 75 /* sectors (frames) a second of audio */

typedef struct {
    uint8_t first, last; /* track numbers */
    struct {
        uint32_t lba;       /* where it starts */
        bool data;          /* a data track (not audio) */
    } t[CD_MAX_TRACKS + 1]; /* by track number; t[last + 1]: the end */
} cd_toc_t;

typedef enum {
    CD_AUDIO_NONE, /* no play going on */
    CD_AUDIO_PLAYING,
    CD_AUDIO_PAUSED,
    CD_AUDIO_DONE,  /* ran to the end */
    CD_AUDIO_ERROR, /* stopped by an error */
} cd_audio_t;

bool cd_read_toc(cd_toc_t *toc); /* READ TOC */
/* play sectors [from, to) (PLAY AUDIO MSF) */
bool cd_play(uint32_t from, uint32_t to);
bool cd_pause(bool pause); /* PAUSE / RESUME */
bool cd_stop_audio(void);
/* where the play is (READ SUB-CHANNEL): its state, the sector */
cd_audio_t cd_audio_status(uint32_t *lba);

#endif
