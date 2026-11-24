// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef APPLOAD_H
#define APPLOAD_H

/*
 * .APP header bytes (v1=68, v2=80):
 *   0:4 magic; 4:2 version; 6:2 flags (bit0 single-instance; bit1 v2
 *   window-first state; bits8-15 heap in 16 KB units, 0=64 KB, max ~4 MB).
 *   8:4 header size; 12:4 code offset; 16:4 code/data size;
 *   20:4 state size (4..ASMA_MAX_STATE); 24:32 NUL name;
 *   56/60/64:4 init/frame/destroy offsets (0 valid; v1 destroy 0 means none,
 *   v2 uses ASMA_NO_ENTRY).
 * v2 adds 68:4 relocation offset, 72:4 count, 76:4 zero-filled BSS.
 * A v2 header of 84 bytes or more says what the app needs (older ASMOS
 * skips it): 80:1 the least CPU (3 386, 4 486, 5 Pentium; 0 any), 81:1 0,
 * 82:2 the least RAM in 64 KB units (0 any).
 * Callbacks receive state (v1) or (api,state) (v2). Link v2 at 0;
 * mkapp.py builds relocations from the ELF.
 */

#include "fs/fs.h"
#include "lib/core.h"
#include "os/os.h"

#define ASMA_MAGIC 0x414D5341u
#define ASMA_VERSION_1 1
#define ASMA_VERSION_2 2
#define ASMA_FLAG_SINGLE (1u << 0)
#define ASMA_FLAG_WINDOW_FIRST (1u << 1)
#define ASMA_HEAP_SHIFT 8
#define ASMA_HEAP_MASK 0xFF00u /* heap size, 16 KB units */
#define ASMA_FLAGS_KNOWN                                                       \
    (ASMA_FLAG_SINGLE | ASMA_FLAG_WINDOW_FIRST | ASMA_HEAP_MASK)

#define ASMA_NAME_MAX 32
#define ASMA_HEADER_V1 68
#define ASMA_HEADER_V2 80
#define ASMA_HEADER_NEEDS 84 /* v2 with the requirements */

typedef struct __attribute__((packed)) {
    uint8_t cpu; /* the least CPU family: 3, 4, 5 (0 any) */
    uint8_t zero;
    uint16_t ram_64k; /* the least RAM, in 64 KB (0 any) */
} asma_needs_t;
#define ASMA_NO_ENTRY 0xFFFFFFFFu

#define ASMA_MAX_CODE_SIZE (256u * 1024u)
#define ASMA_MAX_BSS (256u * 1024u)
#define ASMA_MAX_STATE (64u * 1024u)
#define ASMA_MAX_RELOCS 32768u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t header_size;
    uint32_t code_offset;
    uint32_t code_size;
    uint32_t state_size;
    char name[ASMA_NAME_MAX];
    uint32_t entry_init;
    uint32_t entry_frame;
    uint32_t entry_destroy;
    uint32_t reloc_offset;
    uint32_t reloc_count;
    uint32_t bss_size;
} asma_header_t;

#define MAX_LOADED_APPS 32

typedef struct {
    asma_header_t hdr;
    uint8_t *raw;
    uint8_t *code_base;
    uint32_t image_size;
    uint32_t img_pages; /* image in the frame pool (0: in the kernel heap) */
    app_descriptor desc;
    bool loaded;
    bool faulted;
    bool was_run;
    uint32_t file_size;
    uint16_t mod_date, mod_time;
    char path[FS_PATH_SZ];
} loaded_app_t;

/* The .APP in /APPS (or one of its folders) whose header name is `name`
 * (any case): its path in out. */
bool appload_find_named(const char *name, char *out, int sz);
/* the .APP an app was loaded from (NULL: built into the kernel) */
const char *appload_path_of(const app_descriptor *d);
/* Run the app called `name` from /APPS; with `file`, ask it to open that
 * file (api->open_request). False if there is no such app. */
bool appload_run_named(const char *name, const char *file);

/* Load (or reuse / hot-reload if the file changed) and register. */
app_descriptor *appload_load(const char *path);
app_instance_t *appload_launch(const char *path);
/* Refuses (returns false) if any instance is running. */
bool appload_unload(app_descriptor *desc);
/* Free images of dynamic apps whose instances have all exited. */
void appload_gc(void);
/* Load every *.APP in dir. Returns count loaded. */
int appload_scan_dir(const char *dir_path);
/* the app's name from a .APP header, without loading it */
bool appload_peek_name(const char *path, char *out, int max);
app_descriptor *appload_find(const char *name);
/* Try path as-is, then common locations. */
bool appload_try_open(const char *path_or_name);
/* Last human-readable failure reason ("" if none). */
const char *appload_last_error(void);

#endif
