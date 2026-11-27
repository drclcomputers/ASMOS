// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef FS_ZIP_H
#define FS_ZIP_H

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

/* only the entries want says yes to (name: as in the zip, "APPS/A.APP") */
typedef bool (*zip_want_fn)(void *user, const char *name);
const char *zip_extract_some(const char *zip_path, const char *dst_dir,
                             bool overwrite, zip_want_fn want, void *want_user,
                             zip_progress_fn cb, void *user, zip_stats_t *st);

/* the first bytes look like a zip (or an empty one) */
bool zip_is_zip(const char *path);

uint32_t crc32_update(uint32_t crc, const void *p, uint32_t n);

#endif
