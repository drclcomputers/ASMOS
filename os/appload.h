#ifndef APPLOAD_H
#define APPLOAD_H

/*
 * ASMOS Dynamic Application Loader
 * --------------------------------
 * Single-file packages with the .APP extension (same extension the desktop
 * already understands).  A package is a small header followed by the raw
 * machine code of the application.
 *
 * Layout of an .APP file:
 *
 *   Offset  Size   Field
 *   0       4      magic          "ASMA"
 *   4       2      version        currently 1
 *   6       2      flags          bit 0 = single_instance
 *   8       4      header_size    sizeof(asma_header_t)
 *  12       4      code_offset    from start of file
 *  16       4      code_size      bytes of code+data
 *  20       4      state_size     bytes the app needs for its private state
 *  24      32      name           null-terminated display name
 *  56       4      entry_init     offset of init(void *state) relative to code
 *  60       4      entry_frame    offset of on_frame(void *state)
 *  64       4      entry_destroy  offset of destroy(void *state)  (0 = none)
 *  68       ...    reserved / future (icon, etc.)
 *  header_size ... code + data
 *
 * The binary is loaded into a private region (or kernel heap for the first
 * version) and the three entry points are called exactly like a normal
 * statically-linked app_descriptor.
 *
 * Apps are expected to live under /apps (or the desktop folder).  Installing
 * a new app is simply copying the .APP file there — no kernel rebuild, no
 * reboot required.
 */

#include "lib/core.h"
#include "os/os.h"

#define ASMA_MAGIC          0x414D5341u   /* 'ASMA' little-endian */
#define ASMA_VERSION        1
#define ASMA_FLAG_SINGLE    (1u << 0)

#define ASMA_NAME_MAX       32
#define ASMA_HEADER_SIZE    68
#define ASMA_MAX_CODE_SIZE  (256 * 1024)  /* hard safety limit */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t header_size;     /* must be ASMA_HEADER_SIZE */
    uint32_t code_offset;
    uint32_t code_size;
    uint32_t state_size;
    char     name[ASMA_NAME_MAX];
    uint32_t entry_init;      /* offset from start of code blob */
    uint32_t entry_frame;
    uint32_t entry_destroy;
} asma_header_t;

/* Runtime representation of a loaded package */
typedef struct {
    asma_header_t   hdr;
    uint8_t        *code_base;     /* where the code was loaded */
    uint32_t        code_size;
    void           *state;         /* owned by the app_instance */
    app_descriptor  desc;          /* synthetic descriptor we hand to os_ */
    bool            loaded;
    char            path[256];     /* original file path (for debugging) */
} loaded_app_t;

/* Maximum number of simultaneously loaded dynamic apps */
#define MAX_LOADED_APPS  32

/* Load a .APP file from the filesystem and register it so os_launch_app works.
 * Returns the synthetic app_descriptor on success, NULL on failure.
 * The descriptor stays valid until appload_unload() is called. */
app_descriptor *appload_load(const char *path);

/* Convenience: load + immediately launch. */
app_instance_t *appload_launch(const char *path);

/* Unload a previously loaded package (only safe if no instances are running). */
void appload_unload(app_descriptor *desc);

/* Scan a directory (e.g. "/apps") and load every .APP found.
 * Returns how many packages were successfully loaded. */
int appload_scan_dir(const char *dir_path);

/* Lookup by name among dynamically loaded packages. */
app_descriptor *appload_find(const char *name);

/* Called from the desktop open_item path when a real .APP file is clicked. */
bool appload_try_open(const char *path_or_name);

#endif /* APPLOAD_H */
