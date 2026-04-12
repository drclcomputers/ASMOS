#ifndef LIBC_COMPAT_H
#define LIBC_COMPAT_H

#include "lib/types.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "lib/alloc.h"
#include "lib/libcore.h"
#include "fs/fat16.h"

#ifndef EOF
#define EOF (-1)
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define NULL ((void*)0)

typedef struct {
    fat16_file_t f;
    bool         is_open;
    bool         is_write;
    bool         is_stderr;
    bool         eof;
    bool         error;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
int    fflush(FILE *f);
char  *fgets(char *buf, int n, FILE *f);
int    fgetc(FILE *f);
int    fputc(int c, FILE *f);
int    fputs(const char *s, FILE *f);
int    fprintf(FILE *f, const char *fmt, ...);
int    printf(const char *fmt, ...);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    snprintf(char *s, size_t n, const char *fmt, ...);
int    vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
void   perror(const char *s);
int    remove(const char *path);
int    rename(const char *old, const char *newname);

/* ── memory ── */
void  *malloc(size_t size);
void  *calloc(size_t count, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);


#define assert(cond) \
    do { if (!(cond)) { \
        fprintf(stderr, "assert failed: %s line %d\n", __FILE__, __LINE__); \
        abort(); \
    } } while(0)

#endif
