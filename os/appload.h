#ifndef APPLOAD_H
#define APPLOAD_H

/*
 * ASMOS dynamic application loader (.APP packages)
 *
 *   Off  Size  Field
 *   0    4     magic          'ASMA'
 *   4    2     version        1 (legacy) or 2
 *   6    2     flags          bit0 single_instance
 *                             bit1 state starts with a window* (v2 only)
 *   8    4     header_size    68 (v1) or 80 (v2)
 *   12   4     code_offset    from file start
 *   16   4     code_size      bytes of code+data stored in the file
 *   20   4     state_size     4 .. ASMA_MAX_STATE
 *   24   32    name           NUL-terminated
 *   56   4     entry_init     offset into code (0 is valid)
 *   60   4     entry_frame    offset into code (0 is valid)
 *   64   4     entry_destroy  offset into code; v2: ASMA_NO_ENTRY = none,
 *                             v1: 0 = none
 *   --- v2 only ---
 *   68   4     reloc_offset   from file start; table of u32 code offsets
 *   72   4     reloc_count    each word at code+off gets += load base
 *   76   4     bss_size       zero-filled bytes appended after the code
 *
 * v1: entry(state).        v2: entry(const os_api_t *api, void *state).
 * Link v2 apps at address 0 (-Ttext 0 --emit-relocs); tools/mkapp.py
 * builds the relocation table from the ELF.
 */

#include "fs/fs.h"
#include "lib/core.h"
#include "os/os.h"

#define ASMA_MAGIC 0x414D5341u
#define ASMA_VERSION_1 1
#define ASMA_VERSION_2 2
#define ASMA_FLAG_SINGLE (1u << 0)
#define ASMA_FLAG_WINDOW_FIRST (1u << 1)
#define ASMA_FLAGS_KNOWN (ASMA_FLAG_SINGLE | ASMA_FLAG_WINDOW_FIRST)

#define ASMA_NAME_MAX 32
#define ASMA_HEADER_V1 68
#define ASMA_HEADER_V2 80
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
