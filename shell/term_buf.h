// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef SHELL_TERM_BUF_H
#define SHELL_TERM_BUF_H

#include "lib/core.h"

#define TERM_BUF_LINES 256
#define TERM_BUF_LINE_W 128

void term_buf_push(const char *line);
void term_buf_push_text(const char *text);
int term_buf_count(void);
const char *term_buf_get(int i);
void term_buf_clear(void);
void term_buf_free(void); /* release heap buffer (e.g. if shell exits) */
int term_buf_read_all(char *dst, int max);
int term_buf_read_new(char *dst, int max, int *cursor);
bool term_buf_save(const char *path);

typedef struct term_context term_context_t;

/* Non-character keys delivered by getchar() (above the ASCII range, so
 * they can't be confused with typed letters; raw scancodes used to be
 * passed, and 0x48 "up" was also the letter 'H'). */
#define TERM_KEY_UP 0x80
#define TERM_KEY_DOWN 0x81
#define TERM_KEY_LEFT 0x82
#define TERM_KEY_RIGHT 0x83
#define TERM_KEY_PGUP 0x84
#define TERM_KEY_PGDN 0x85
#define TERM_KEY_ESC 0x86
#define TERM_KEY_HOME 0x87
#define TERM_KEY_END 0x88

struct term_context {
    void (*print)(term_context_t *ctx, const char *str);
    void (*putchar)(term_context_t *ctx, char c);
    int (*readline)(term_context_t *ctx, char *buf, int maxchars);
    int (*getchar)(term_context_t *ctx);
    void *userdata;
    /* optional (may be NULL): */
    void (*clear)(term_context_t *ctx); /* wipe the output area */
    void (*size)(term_context_t *ctx, int *cols, int *rows); /* visible */
};

static inline void term_ctx_print(term_context_t *ctx, const char *s) {
    if (ctx && ctx->print)
        ctx->print(ctx, s);
}

void term_ctx_printf(term_context_t *ctx, const char *fmt, ...);

#endif
