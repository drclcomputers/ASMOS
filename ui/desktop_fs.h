#ifndef DESKTOP_FS_H
#define DESKTOP_FS_H

#include "fs/fat16.h"
#include "lib/core.h"

#define DESKTOP_PATH "/DESKTOP"
#define DESKTOP_CLUSTER_NONE 0xFFFF

#define DESKTOP_ITEM_FILE 0
#define DESKTOP_ITEM_DIR 1
#define DESKTOP_ITEM_APP 2
#define DESKTOP_ITEM_FDD 3
#define DESKTOP_ITEM_HDD DESKTOP_ITEM_FDD

#define DESKTOP_MAX_ITEMS 32
#define DESKTOP_NAME_MAX 13
#define DESKTOP_APP_EXT "APP"

typedef struct {
    char name[DESKTOP_NAME_MAX];
    uint8_t kind;
    bool used;
    uint8_t drive_id;

    int x, y;
    bool selected;
    bool dragging;
    int drag_off_x, drag_off_y;
} desktop_item_t;

void desktop_fs_init(void);
void desktop_fs_reload(void);
void desktop_fs_set_dirty(void);
bool desktop_fs_is_dirty(void);

desktop_item_t *desktop_fs_items(void);
int desktop_fs_count(void);
bool desktop_fs_add_app(const char *app_name);
bool desktop_fs_delete(int idx);
bool desktop_fs_rename(int idx, const char *new_name);
void desktop_fs_move_icon(int idx, int new_x, int new_y);

uint16_t desktop_fs_cluster(void);
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
#define SMECHER_ASM                                                            \
    "[BITS 32]\n"                                                              \
    "_start:\n"                                                                \
    "    push ebx               ; Preserve EBX (C-standard)\n"                 \
    "    mov eax, [esp + 8]     ; Get syscall_t pointer (now at +8 due to "    \
    "push)\n"                                                                  \
    "\n"                                                                       \
    "    push 33                ; Push '!' (ASCII 33)\n"                       \
    "    mov ebx, [eax + 4]     ; Load address of sc_putchar into EBX\n"       \
    "    call ebx               ; Call it\n"                                   \
    "    add esp, 4             ; Clean stack\n"                               \
    "\n"                                                                       \
    "    pop ebx                ; Restore EBX\n"                               \
    "    ret"

#define HELLO_ASM                                                              \
    "[BITS 32]\n"                                                              \
    "_start:\n"                                                                \
    "    push ebp\n"                                                           \
    "    mov ebp, esp\n"                                                       \
    "    sub esp, 32            ; Allocate 32 bytes on stack for a buffer\n"   \
    "    push ebx\n"                                                           \
    "    push esi\n"                                                           \
    "    push edi               ; Preserve EDI! (We use it as a counter)\n"    \
    "\n"                                                                       \
    "    mov esi, [ebp + 8]     ; ESI = bin_syscall_t pointer\n"               \
    "\n"                                                                       \
    "    ; 1. The ASMOS Base Address Trick\n"                                  \
    "    call .here\n"                                                         \
    ".here:\n"                                                                 \
    "    pop ebx                ; EBX = absolute address of .here in RAM\n"    \
    "    sub ebx, (.here - $$)  ; EBX = base address of the binary\n"          \
    "\n"                                                                       \
    "    ; 2. Print Prompt\n"                                                  \
    "    ; Since EBX is our base, and 'prompt' is an offset, this parses "     \
    "perfectly!\n"                                                             \
    "    lea eax, [ebx + prompt]\n"                                            \
    "    push eax\n"                                                           \
    "    call [esi + 0]\n"                                                     \
    "    add esp, 4\n"                                                         \
    "\n"                                                                       \
    "    ; 3. Read Input\n"                                                    \
    "    push 31                ; Max chars\n"                                 \
    "    lea eax, [ebp - 32]    ; Our stack buffer\n"                          \
    "    push eax\n"                                                           \
    "    call [esi + 8]         ; Will NOT freeze now that the IF bug is "     \
    "fixed!\n"                                                                 \
    "    add esp, 8\n"                                                         \
    "\n"                                                                       \
    "    ; 4. Greeting Loop\n"                                                 \
    "    mov edi, 1\n"                                                         \
    ".loop:\n"                                                                 \
    "    lea eax, [ebp - 32]    \n"                                            \
    "    push eax\n"                                                           \
    "    push edi\n"                                                           \
    "    call [esi + 16]        ; sc_itoa\n"                                   \
    "    add esp, 8\n"                                                         \
    "\n"                                                                       \
    "    lea eax, [ebp - 32]\n"                                                \
    "    push eax\n"                                                           \
    "    call [esi + 0]         ; sc_print (the number)\n"                     \
    "    add esp, 4\n"                                                         \
    "\n"                                                                       \
    "    push 32                ; ASCII space\n"                               \
    "    call [esi + 4]         ; sc_putchar\n"                                \
    "    add esp, 4\n"                                                         \
    "\n"                                                                       \
    "    inc edi\n"                                                            \
    "    cmp edi, 6\n"                                                         \
    "    jne .loop\n"                                                          \
    "\n"                                                                       \
    "    ; 5. Exit cleanly\n"                                                  \
    "    push 10                ; '\\n'\n"                                     \
    "    call [esi + 4]\n"                                                     \
    "    add esp, 4\n"                                                         \
    "\n"                                                                       \
    "    pop edi                ; Restore EDI!\n"                              \
    "    pop esi\n"                                                            \
    "    pop ebx\n"                                                            \
    "    mov esp, ebp\n"                                                       \
    "    pop ebp\n"                                                            \
    "    ret\n"                                                                \
    "\n"                                                                       \
    "prompt: db \"Enter Name: \", 0\n"

#endif
