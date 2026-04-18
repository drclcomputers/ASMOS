#ifndef STRING_H
#define STRING_H

#include "lib/core.h"

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, l) __builtin_va_arg(v, l)

size_t strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

size_t strlcpy(char *dst, const char *src, size_t dstsz);
size_t strlcat(char *dst, const char *src, size_t dstsz);
char *strtok_r(char *str, const char *delim, char **saveptr);

// ── Memory search
// ─────────────────────────────────────────────────────────────

void *memchr(const void *s, int c, size_t n);
void *memmem(const void *haystack, size_t hlen, const void *needle,
             size_t nlen);

char *uint32_to_str(uint32_t num, char *str);
uint32_t str_to_uint32(const char *str);
int str_to_int(const char *str);
char *int_to_str(int num, char *str);
void uint32_to_hex_str(uint32_t num, char *str, bool uppercase);

long str_to_long(const char *s, char **end, int base);
unsigned long str_to_ulong(const char *s, char **end, int base);
double str_to_double(const char *s, char **end);

// Formatted output

int vsprintf(char *str, const char *format, va_list args);
int sprintf(char *str, const char *format, ...);

int isalpha(int c);
int isdigit(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isalnum(int c);
int isprint(int c);
int ispunct(int c);
int isxdigit(int c);
int iscntrl(int c);
int toupper(int c);
int tolower(int c);

// String builder
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} strbuf_t;
static inline void strbuf_init(strbuf_t *sb, char *buf, size_t cap) {
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = false;
    if (cap > 0)
        buf[0] = '\0';
}
static inline bool strbuf_append(strbuf_t *sb, const char *s) {
    if (!s)
        return false;
    size_t slen = strlen(s);
    size_t avail = (sb->cap > sb->len + 1) ? (sb->cap - sb->len - 1) : 0;
    size_t copy = (slen < avail) ? slen : avail;
    if (copy > 0) {
        for (size_t i = 0; i < copy; i++)
            sb->buf[sb->len + i] = s[i];
        sb->len += copy;
        sb->buf[sb->len] = '\0';
    }
    if (slen > avail)
        sb->overflow = true;
    return !sb->overflow;
}
static inline bool strbuf_append_char(strbuf_t *sb, char c) {
    if (sb->len + 1 < sb->cap) {
        sb->buf[sb->len++] = c;
        sb->buf[sb->len] = '\0';
        return true;
    }
    sb->overflow = true;
    return false;
}
static inline bool strbuf_append_int(strbuf_t *sb, int v) {
    char tmp[16];
    int_to_str(v, tmp);
    return strbuf_append(sb, tmp);
}
static inline bool strbuf_append_uint(strbuf_t *sb, uint32_t v) {
    char tmp[16];
    uint32_to_str(v, tmp);
    return strbuf_append(sb, tmp);
}
static inline bool strbuf_append_fmt(strbuf_t *sb, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(tmp, fmt, ap);
    va_end(ap);
    return strbuf_append(sb, tmp);
}
static inline void strbuf_clear(strbuf_t *sb) {
    sb->len = 0;
    sb->overflow = false;
    if (sb->cap > 0)
        sb->buf[0] = '\0';
}
static inline const char *strbuf_str(const strbuf_t *sb) { return sb->buf; }
static inline size_t strbuf_len(const strbuf_t *sb) { return sb->len; }
static inline void strbuf_rtrim(strbuf_t *sb) {
    while (sb->len > 0 && isspace((unsigned char)sb->buf[sb->len - 1]))
        sb->buf[--sb->len] = '\0';
}

const char *path_basename(const char *path);
void path_dirname(const char *path, char *dir, size_t dir_sz);
void path_join(const char *base, const char *rel, char *out, size_t out_sz);
const char *path_ext(const char *path);
static inline bool path_is_absolute(const char *path) {
    return path && path[0] == '/';
}

#endif
