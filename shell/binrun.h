#ifndef SHELL_BINRUN_H
#define SHELL_BINRUN_H

#include "lib/core.h"
#include "shell/term_buf.h"

/* Sentinel for NUL bytes inside string tokens — must not appear in ASM source */
#define ASM_NUL_SENTINEL ((char)0x01)

/* The binary receives a pointer to this struct as its first cdecl argument
 * ([esp+4] on entry).
 *
 * ASM constants:
 *   SC_PRINT    equ 0   ; void print(const char *str)
 *   SC_PUTCHAR  equ 4   ; void putchar(char c)
 *   SC_READLINE equ 8   ; int  readline(char *buf, int maxchars)
 *   SC_GETCHAR  equ 12  ; int  getchar(void)
 *   SC_ITOA     equ 16  ; void itoa(int val, char *buf_12bytes)
 */
typedef struct {
    void (*sc_print)   (const char *str);
    void (*sc_putchar) (char c);
    int  (*sc_readline)(char *buf, int maxchars);
    int  (*sc_getchar) (void);
    void (*sc_itoa)    (int val, char *buf);
} bin_syscall_t;

const bin_syscall_t *binrun_make_table(term_context_t *ctx);

void cmd_run (term_context_t *ctx, const char *path, char *out, size_t max);

void cmd_run_cli(const char *path, char *out, size_t max);

#endif
