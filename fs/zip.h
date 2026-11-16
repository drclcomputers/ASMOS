#ifndef FS_ZIP_H
#define FS_ZIP_H

/*
 * ZIP archives: make one from a file or folder, and take one apart.
 *
 * Standard PKZIP 2.0 files (method 0 "stored" and 8 "deflate"), so they
 * open in Windows, Info-ZIP, PKUNZIP 2.04g, 7-Zip... and their zips open
 * here. Not handled: encrypted entries, ZIP64 (files over 4 GB), and other
 * methods (shrink, implode, bzip2...): those entries are skipped and
 * counted.
 *
 * Paths are the usual absolute ones ("/HDA/DOCS", "/FDD0/A.ZIP"). Long
 * names go into the archive as they are; names coming out are made safe
 * for FAT (bad characters become '_', ".." parts are dropped, so nothing
 * lands outside the folder you extract into).
 *
 * Both run to the end before returning (a floppy job takes a while: the
 * progress callback is the place to draw something).
 */

#include "lib/core.h"

enum { ZIP_STORE, ZIP_FAST, ZIP_NORMAL, ZIP_BEST };

typedef struct {
    int files, dirs;
    int skipped;     /* extract: already there (no overwrite) */
    int unsupported; /* extract: encrypted / unknown method / damaged name */
    int bad;         /* extract: CRC or data errors (file removed) */
    uint32_t in_bytes, out_bytes; /* create: read, written; extract: same */
    bool stored_only; /* create: not enough memory to compress, stored */
} zip_stats_t;

/* done/total: bytes of file data (uncompressed) */
typedef void (*zip_progress_fn)(void *user, const char *name, uint32_t done,
                                uint32_t total);

/* Returns NULL when it worked, else a short reason. The zip is removed if
 * it fails part way. */
const char *zip_create(const char *src, const char *zip_path, int level,
                       bool hidden, zip_progress_fn cb, void *user,
                       zip_stats_t *st);
const char *zip_extract(const char *zip_path, const char *dst_dir,
                        bool overwrite, zip_progress_fn cb, void *user,
                        zip_stats_t *st);

/* the first bytes look like a zip (or an empty one) */
bool zip_is_zip(const char *path);

uint32_t crc32_update(uint32_t crc, const void *p, uint32_t n);

#endif
