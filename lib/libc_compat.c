#include "lib/libc_compat.h"
#include "lib/alloc.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/time.h"
#include "fs/fat16.h"
#include "os/os.h"

/* ── stdin / stdout / stderr ──────────────────────────────────────── */

static FILE s_stdout = { .is_open = true, .is_write = true,  .is_stderr = false };
static FILE s_stderr = { .is_open = true, .is_write = true,  .is_stderr = true  };
static FILE s_stdin  = { .is_open = true, .is_write = false, .is_stderr = false };

FILE *stdout = &s_stdout;
FILE *stderr = &s_stderr;
FILE *stdin  = &s_stdin;

static char s_term_buf[4096];
static int  s_term_len = 0;

static void term_putchar(char c) {
    if (s_term_len < (int)sizeof(s_term_buf) - 1) {
        s_term_buf[s_term_len++] = c;
        s_term_buf[s_term_len]   = '\0';
    }
}

static void term_puts(const char *s) {
    while (*s) term_putchar(*s++);
}

const char *libc_term_get_buf(void)  { return s_term_buf; }
void        libc_term_clear_buf(void){ s_term_len = 0; s_term_buf[0] = '\0'; }

/* ── FILE I/O ─────────────────────────────────────────────────────── */

FILE *fopen(const char *path, const char *mode) {
    FILE *f = (FILE *)kmalloc(sizeof(FILE));
    if (!f) return NULL;
    memset(f, 0, sizeof(FILE));

    bool write_mode = (mode[0] == 'w' || mode[0] == 'a' ||
                       (mode[0] == 'r' && mode[1] == '+'));

    f->is_write = write_mode;

    if (mode[0] == 'w') {
        dir_entry_t de;
        if (fat16_find(path, &de)) fat16_delete(path);
        if (!fat16_create(path, &f->f)) { kfree(f); return NULL; }
    } else if (mode[0] == 'a') {
        dir_entry_t de;
        if (fat16_find(path, &de)) {
            if (!fat16_open(path, &f->f)) { kfree(f); return NULL; }
            fat16_seek(&f->f, f->f.entry.file_size);
        } else {
            if (!fat16_create(path, &f->f)) { kfree(f); return NULL; }
        }
    } else {
        if (!fat16_open(path, &f->f)) { kfree(f); return NULL; }
    }

    f->is_open = true;
    return f;
}
int fclose(FILE *f) {
    if (!f || !f->is_open) return EOF;
    if (f == stdin || f == stdout || f == stderr) return 0;
    fat16_close(&f->f);
    f->is_open = false;
    kfree(f);
    return 0;
}
size_t fread(void *buf, size_t size, size_t count, FILE *f) {
    if (!f || !f->is_open || f->is_write) return 0;
    if (f == stdin) return 0;  /* no stdin support */
    int total = (int)(size * count);
    int n = fat16_read(&f->f, buf, total);
    if (n < total) f->eof = true;
    if (n < 0)   { f->error = true; return 0; }
    return (size_t)(n / (size ? size : 1));
}
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f) {
    if (!f || !f->is_open) return 0;
    int total = (int)(size * count);
    if (f == stdout || f == stderr || f->is_stderr) {
        const char *p = (const char *)buf;
        for (int i = 0; i < total; i++) term_putchar(p[i]);
        return count;
    }
    int n = fat16_write(&f->f, buf, total);
    if (n < 0) { f->error = true; return 0; }
    return (size_t)(n / (size ? size : 1));
}
int fseek(FILE *f, long offset, int whence) {
    if (!f || !f->is_open) return -1;
    if (f == stdin || f == stdout || f == stderr) return -1;
    uint32_t target;
    switch (whence) {
        case SEEK_SET: target = (uint32_t)offset; break;
        case SEEK_CUR: target = f->f.cur_offset + (uint32_t)offset; break;
        case SEEK_END: target = f->f.entry.file_size + (uint32_t)offset; break;
        default: return -1;
    }
    f->eof = false;
    return fat16_seek(&f->f, target) ? 0 : -1;
}
long ftell(FILE *f) {
    if (!f || !f->is_open) return -1;
    if (f == stdin || f == stdout || f == stderr) return -1;
    return (long)fat16_tell(&f->f);
}
int feof(FILE *f)   { return f && f->eof   ? 1 : 0; }
int ferror(FILE *f) { return f && f->error ? 1 : 0; }
int fflush(FILE *f) { (void)f; return 0; }
char *fgets(char *buf, int n, FILE *f) {
    if (!f || !f->is_open || n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return NULL; break; }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}
int fgetc(FILE *f) {
    if (!f || !f->is_open) return EOF;
    if (f == stdin) return EOF;
    uint8_t c;
    int n = fat16_read(&f->f, &c, 1);
    if (n <= 0) { f->eof = true; return EOF; }
    return (int)c;
}
int fputc(int c, FILE *f) {
    char ch = (char)c;
    fwrite(&ch, 1, 1, f);
    return c;
}
int fputs(const char *s, FILE *f) {
    if (!s) return EOF;
    int len = (int)strlen(s);
    return (int)fwrite(s, 1, len, f);
}
int fprintf(FILE *f, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    fwrite(buf, 1, n, f);
    return n;
}
int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    fwrite(buf, 1, n, stdout);
    return n;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsprintf(buf, fmt, ap);
    fwrite(buf, 1, n, f);
    return n;
}
int snprintf(char *s, size_t n, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsprintf(buf, fmt, ap);
    va_end(ap);
    if (len >= (int)n) len = (int)n - 1;
    memcpy(s, buf, len);
    s[len] = '\0';
    return len;
}
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap) {
    char buf[1024];
    int len = vsprintf(buf, fmt, ap);
    if (len >= (int)n) len = (int)n - 1;
    memcpy(s, buf, len);
    s[len] = '\0';
    return len;
}
void perror(const char *s) {
    fprintf(stderr, "%s: error\n", s ? s : "");
}
int remove(const char *path) {
    return fat16_delete(path) ? 0 : -1;
}

int rename(const char *old, const char *newname) {
    const char *base = strrchr(newname, '/');
    base = base ? base + 1 : newname;
    return fat16_rename(old, base) ? 0 : -1;
}

/* ── memory ───────────────────────────────────────────────────────── */

void *malloc(size_t size)            { return kmalloc((uint32_t)size); }
void  free(void *ptr)                { kfree(ptr); }

void *calloc(size_t count, size_t size) {
    void *p = kmalloc((uint32_t)(count * size));
    if (p) memset(p, 0, count * size);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc((uint32_t)size);
    if (size == 0) { kfree(ptr); return NULL; }
    void *newp = kmalloc((uint32_t)size);
    if (!newp) return NULL;
    memcpy(newp, ptr, size);
    kfree(ptr);
    return newp;
}
