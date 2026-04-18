#ifndef LIBC_COMPAT_H
#define LIBC_COMPAT_H

// ibc compatibility layer
// Provides a stdlib/stdio/string interface that TCC and NASM-compiled code can
// link against without pulling in a real libc.

#include "fs/fat16.h"
#include "lib/compat/libcore.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/string.h"

#ifndef EOF
#define EOF (-1)
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifndef NULL
#define NULL ((void *)0)
#endif

// FILE

typedef struct {
    fat16_file_t f;
    bool is_open;
    bool is_write;
    bool is_stderr;
    bool eof;
    bool error;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int fflush(FILE *f);
void clearerr(FILE *f);
char *fgets(char *buf, int n, FILE *f);
int fgetc(FILE *f);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int fprintf(FILE *f, const char *fmt, ...);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int snprintf(char *s, size_t n, const char *fmt, ...);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
void perror(const char *s);
int remove(const char *path);
int rename(const char *old, const char *newname);
int puts(const char *s);
int putchar(int c);
int getchar(void);

const char *libc_term_get_buf(void);
void libc_term_clear_buf(void);

// memory

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

// number conversions

static inline int atoi(const char *s) { return str_to_int(s); }
static inline long atol(const char *s) {
    return (long)str_to_long(s, NULL, 10);
}
static inline double atof(const char *s) { return str_to_double(s, NULL); }
static inline long strtol(const char *s, char **e, int b) {
    return str_to_long(s, e, b);
}
static inline unsigned long strtoul(const char *s, char **e, int b) {
    return str_to_ulong(s, e, b);
}
static inline double strtod(const char *s, char **e) {
    return str_to_double(s, e);
}

// Aliases matching libc names
#define strtol str_to_long
#define strtoul str_to_ulong
#define strtod str_to_double
#define atoi(s) str_to_int(s)
#define atol(s) ((long)str_to_long((s), NULL, 10))
#define atof(s) str_to_double((s), NULL)

static inline char *itoa(int v, char *buf, int base) {
    if (base == 10) {
        int_to_str(v, buf);
        return buf;
    }
    if (base == 16) {
        uint32_to_hex_str((uint32_t)v, buf, false);
        return buf;
    }
    buf[0] = '0';
    buf[1] = '\0';
    return buf;
}

int abs(int x);
void exit(int code);
void abort(void);
char *getenv(const char *name);
void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

#define assert(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "assert failed: %s line %d\n", __FILE__,           \
                    __LINE__);                                                 \
            abort();                                                           \
        }                                                                      \
    } while (0)

#endif
