// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef ISO9660_H
#define ISO9660_H

#include "fs/fs.h"

/*
 * Read-only ISO 9660 with optional Joliet names, exposed by fs/fs.c as /CD0.
 * Directories use small handles (0 is root); file entries store the first
 * sector. Both the disc name and an 8.3 alias are available.
 */

typedef bool (*iso_cb)(dir_entry_t *e, const char *lname, void *user);

bool iso_mount(fat_vol_t *v); /* the disc in the drive; label, size */
void iso_unmount(void);
bool iso_iterate(uint16_t dir, iso_cb cb, void *user); /* "." and ".." too */
int iso_read(fat_file_t *f, void *buf, int len);
uint32_t iso_size_bytes(void);

/* an audio CD: no file system, its tracks as Track01.cda ... (44 bytes:
 * RIFF "CDDA", as Windows has them) */
bool iso_is_audio(void);
bool iso_cda_make(int track, uint8_t out[44]); /* that track's file */
/* the track a .cda names is on the disc in the drive (serial 0: any) */
bool iso_cda_check(uint32_t serial, uint32_t start, uint32_t len);

#endif
