#ifndef DESKTOP_FS_H
#define DESKTOP_FS_H

#include "lib/core.h"
#include "fs/fat16.h"

#define DESKTOP_PATH        "/DESKTOP"
#define DESKTOP_CLUSTER_NONE 0xFFFF

#define DESKTOP_ITEM_FILE   0
#define DESKTOP_ITEM_DIR    1
#define DESKTOP_ITEM_APP    2

#define DESKTOP_MAX_ITEMS   32
#define DESKTOP_NAME_MAX    13
#define DESKTOP_APP_EXT     "APP"

typedef struct {
    char    name[DESKTOP_NAME_MAX];
    uint8_t kind;
    bool    used;

    int     x, y;
    bool    selected;
    bool    dragging;
    int     drag_off_x, drag_off_y;
} desktop_item_t;

void desktop_fs_init(void);
void desktop_fs_reload(void);
void desktop_fs_set_dirty(void);
bool desktop_fs_is_dirty(void);

desktop_item_t *desktop_fs_items(void);
int  desktop_fs_count(void);
bool desktop_fs_add_app(const char *app_name);
bool desktop_fs_delete(int idx);
bool desktop_fs_rename(int idx, const char *new_name);
void desktop_fs_move_icon(int idx, int new_x, int new_y);

uint16_t    desktop_fs_cluster(void);
const char *desktop_fs_path(void);

/*
 * HELLO.ASM embedded source — position-independent binary.
 *
 * Calling convention:  void binary(const bin_syscall_t *sc)
 *   [esp+4] on entry = syscall table pointer.
 *
 * Because the binary is loaded at an arbitrary heap address, all data
 * references use the call/pop base-address trick so labels resolve to
 * their actual runtime addresses, not their link-time offsets from 0.
 */
#define HELLO_ASM \
"; The syscall table pointer is at [esp + 4]\n" \
"\n" \
"_start:\n" \
"    mov ebx, [esp + 4]     ; Get the syscall_t pointer\n" \
"\n" \
"    ; Try to print a single character using bsc_putchar\n" \
"    ; Offset for putchar is 4 (see binrun.h)\n" \
"    push '!'               ; Push char to print\n" \
"    call [ebx + 4]         ; Call sc_putchar\n" \
"    add esp, 4             ; Clean up stack\n" \
"\n" \
"    ret                    ; Return to binrun.c\n"

#endif
