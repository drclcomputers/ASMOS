#ifndef SHELL_TERM_BUF_H
#define SHELL_TERM_BUF_H

#include "lib/core.h"

#define TERM_BUF_LINES  256
#define TERM_BUF_LINE_W 128

void        term_buf_push     (const char *line);
void        term_buf_push_text(const char *text);
int         term_buf_count    (void);
const char *term_buf_get      (int i);
void        term_buf_clear    (void);
int         term_buf_read_all (char *dst, int max);
int         term_buf_read_new (char *dst, int max, int *cursor);
bool        term_buf_save     (const char *path);

typedef struct term_context term_context_t;

struct term_context {
    void (*print)   (term_context_t *ctx, const char *str);
    void (*putchar) (term_context_t *ctx, char c);
    int  (*readline)(term_context_t *ctx, char *buf, int maxchars);
    int  (*getchar) (term_context_t *ctx);
    void *userdata;
};

static inline void term_ctx_print(term_context_t *ctx, const char *s) {
    if (ctx && ctx->print) ctx->print(ctx, s);
}

void term_ctx_printf(term_context_t *ctx, const char *fmt, ...);

#endif
