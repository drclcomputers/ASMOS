// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef SHELL_BINRUN_H
#define SHELL_BINRUN_H

#include "lib/core.h"
#include "shell/term_buf.h"

/* Sentinel for NUL bytes inside string tokens — must not appear in ASM source
 */
#define ASM_NUL_SENTINEL ((char)0x01)

/* Binary receives this struct at [esp+4]. ASM service offsets:
 * 0 print(str), 4 putchar(char), 8 readline(buf,max), 12 getchar(),
 * 16 itoa(value,buf). */
typedef struct {
    void (*sc_print)(const char *str);
    void (*sc_putchar)(char c);
    int (*sc_readline)(char *buf, int maxchars);
    int (*sc_getchar)(void);
    void (*sc_itoa)(int val, char *buf);
} bin_syscall_t;

const bin_syscall_t *binrun_make_table(term_context_t *ctx);

void cmd_run(term_context_t *ctx, const char *path, char *out, size_t max);

#endif
