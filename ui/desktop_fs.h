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
"bits 32\n" \
"org  0\n" \
"SC_PRINT    equ 0\n" \
"SC_PUTCHAR  equ 4\n" \
"SC_READLINE equ 8\n" \
"SC_GETCHAR  equ 12\n" \
"SC_ITOA     equ 16\n" \
"    call .findbase\n" \
".findbase:\n" \
"    pop  ebp\n" \
"    sub  ebp, (.findbase - $$)\n" \
"    push ebx\n" \
"    push esi\n" \
"    push edi\n" \
"    mov  ebx, [esp + 16]\n" \
"    lea  eax, [ebp + msg_hello]\n" \
"    push eax\n" \
"    call [ebx + SC_PRINT]\n" \
"    add  esp, 4\n" \
"    lea  eax, [ebp + msg_prompt]\n" \
"    push eax\n" \
"    call [ebx + SC_PRINT]\n" \
"    add  esp, 4\n" \
"    lea  eax, [ebp + name_buf]\n" \
"    push 33\n" \
"    push eax\n" \
"    call [ebx + SC_READLINE]\n" \
"    add  esp, 8\n" \
"    lea  eax, [ebp + msg_reply]\n" \
"    push eax\n" \
"    call [ebx + SC_PRINT]\n" \
"    add  esp, 4\n" \
"    lea  eax, [ebp + name_buf]\n" \
"    push eax\n" \
"    call [ebx + SC_PRINT]\n" \
"    add  esp, 4\n" \
"    lea  eax, [ebp + msg_exclaim]\n" \
"    push eax\n" \
"    call [ebx + SC_PRINT]\n" \
"    add  esp, 4\n" \
"    lea  eax, [ebp + msg_counting]\n" \
"    push eax\n" \
"    call [ebx + SC_PRINT]\n" \
"    add  esp, 4\n" \
"    mov  esi, 1\n" \
".count_loop:\n" \
"    cmp  esi, 5\n" \
"    jg   .count_done\n" \
"    lea  eax, [ebp + num_buf]\n" \
"    push eax\n" \
"    push esi\n" \
"    call [ebx + SC_ITOA]\n" \
"    add  esp, 8\n" \
"    lea  eax, [ebp + num_buf]\n" \
"    push eax\n" \
"    call [ebx + SC_PRINT]\n" \
"    add  esp, 4\n" \
"    cmp  esi, 5\n" \
"    je   .last_num\n" \
"    lea  eax, [ebp + msg_space]\n" \
"    push eax\n" \
"    jmp  .print_sep\n" \
".last_num:\n" \
"    lea  eax, [ebp + msg_newline]\n" \
"    push eax\n" \
".print_sep:\n" \
"    call [ebx + SC_PRINT]\n" \
"    add  esp, 4\n" \
"    inc  esi\n" \
"    jmp  .count_loop\n" \
".count_done:\n" \
"    lea  eax, [ebp + msg_anykey]\n" \
"    push eax\n" \
"    call [ebx + SC_PRINT]\n" \
"    add  esp, 4\n" \
"    call [ebx + SC_GETCHAR]\n" \
"    pop  edi\n" \
"    pop  esi\n" \
"    pop  ebx\n" \
"    ret\n" \
"name_buf:    times 34 db 0\n" \
"num_buf:     times 12 db 0\n" \
"msg_hello:    db \"Hello from ASMOS Assembly!\", 10, 0\n" \
"msg_prompt:   db \"Enter your name: \", 0\n" \
"msg_reply:    db \"Nice to meet you, \", 0\n" \
"msg_exclaim:  db \"!\", 10, 0\n" \
"msg_counting: db \"Counting: \", 0\n" \
"msg_space:    db \" \", 0\n" \
"msg_newline:  db 10, 0\n" \
"msg_anykey:   db \"Press any key to exit...\", 0"

#endif
