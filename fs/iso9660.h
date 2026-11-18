#ifndef ISO9660_H
#define ISO9660_H

#include "fs/fs.h"

/*
 * ISO 9660 CDs (read only), with Joliet long names when the disc has them.
 * fs/fs.c shows the disc as the drive /CD0 through the same calls as the
 * FAT drives: a folder is a small number (0 = the root, like FAT; the
 * others are handed out as folders are seen, iso_dir table), a file
 * carries its first sector in its entry (cluster_hi:cluster_lo). Names:
 * the 8.3 one from the disc's name ("NAME~N.EXT" if it isn't 8.3), the
 * long one as the disc has it.
 */

typedef bool (*iso_cb)(dir_entry_t *e, const char *lname, void *user);

bool iso_mount(fat_vol_t *v); /* the disc in the drive; label, size */
void iso_unmount(void);
bool iso_iterate(uint16_t dir, iso_cb cb, void *user); /* "." and ".." too */
int iso_read(fat_file_t *f, void *buf, int len);
uint32_t iso_size_bytes(void);

#endif
