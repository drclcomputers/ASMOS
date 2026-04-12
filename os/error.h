#ifndef ERROR_H
#define ERROR_H

#include "lib/core.h"

typedef enum {
    ERR_INFO    = 0,
    ERR_WARNING = 1,
    ERR_FATAL   = 2,
} err_severity_t;

typedef enum {
    ERR_OK = 0,

    ERR_ATA_TIMEOUT,
    ERR_ATA_READ,
    ERR_ATA_WRITE,

    ERR_FAT_MOUNT,
    ERR_FAT_NO_SPACE,
    ERR_FAT_CORRUPT,
    ERR_FAT_NOT_FOUND,
    ERR_FAT_EXISTS,
    ERR_FAT_WRITE,
    ERR_FAT_READ,

    ERR_OOM,
    ERR_HEAP_CORRUPT,

    ERR_WM_MAX_WINDOWS,
    ERR_WM_ALLOC,

    ERR_APP_MAX_RUNNING,
    ERR_APP_ALLOC,
    ERR_APP_NOT_FOUND,

    ERR_NULL_PTR,
    ERR_INVALID_ARG,

    ERR_COUNT
} err_code_t;

void error_set_gui_mode(bool gui);
bool error_gui_mode(void);

void error_report(err_severity_t sev, err_code_t code, const char *context);

#define ERR_INFO_REPORT(code, ctx)    error_report(ERR_INFO,    code, ctx)
#define ERR_WARN_REPORT(code, ctx)    error_report(ERR_WARNING, code, ctx)
#define ERR_FATAL_REPORT(code, ctx)   error_report(ERR_FATAL,   code, ctx)

// triggers FATAL if condition is false.
#define ASSERT(cond, code, ctx)  do { if (!(cond)) ERR_FATAL_REPORT(code, ctx); } while(0)
#define ASSERT_NOT_NULL(ptr, ctx) ASSERT((ptr) != NULL, ERR_NULL_PTR, ctx)

// warns but does not halt.
#define WARN_IF(cond, code, ctx) do { if (cond) ERR_WARN_REPORT(code, ctx); } while(0)

void boot_check_ata(void);
void boot_check_fat(void);
void boot_check_heap(void);

#endif
