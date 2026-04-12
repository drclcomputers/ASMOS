#include "lib/string/string.h"
#include "lib/core.h"
#include "lib/memory/alloc.h"
#include "lib/memory/mem.h"

// String functions
size_t strlen(const char* s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    char* d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = 0;
    return dst;
}

char* strcat(char* dst, const char* src) {
    char* d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char* strncat(char* dst, const char* src, size_t n) {
    char* d = dst;
    while (*d) d++;
    while (n-- && (*d++ = *src++));
    *d = 0;
    return dst;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == 0) ? (char*)s : 0;
}

char* strrchr(const char* s, int c) {
    const char* last = 0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    while (*haystack) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
        haystack++;
    }
    return 0;
}

// Conversions to strings and from strings
char* uint32_to_str(uint32_t num, char* str) {
    uint32_t temp = num;
    int len = 0;

    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return str;
    }

    while (temp > 0) {
        temp /= 10;
        len++;
    }

    str[len] = '\0';

    while (num > 0) {
        str[--len] = (num % 10) + '0';
        num /= 10;
    }

    return str;
}

uint32_t str_to_uint32(const char* str) {
    uint32_t res = 0;

    while (*str >= '0' && *str <= '9') {
        res = (res * 10) + (*str - '0');
        str++;
    }

    return res;
}

int str_to_int(const char* str) {
    int res = 0;
    int sign = 1;

    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    while (*str >= '0' && *str <= '9') {
        res = (res * 10) + (*str - '0');
        str++;
    }

    return res * sign;
}

char* int_to_str(int num, char* str) {
    int i = 0;
    int is_negative = 0;
    unsigned int val;

    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return str;
    }

    if (num < 0) {
        is_negative = 1;
        val = (unsigned int)(-num);
    } else {
        val = (unsigned int)num;
    }

    while (val > 0) {
        str[i++] = (val % 10) + '0';
        val /= 10;
    }

    if (is_negative) {
        str[i++] = '-';
    }

    str[i] = '\0';

    int start = 0;
    int end = i - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }

    return str;
}

void uint32_to_hex_str(uint32_t num, char* str, bool uppercase) {
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }

    char temp[16];
    int i = 0;
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    while (num > 0) {
        temp[i++] = digits[num % 16];
        num /= 16;
    }

    int j = 0;
    while (i > 0) {
        str[j++] = temp[--i];
    }
    str[j] = '\0';
}

long str_to_long(const char *s, char **end, int base) {
    while (*s == ' ') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1]=='x'||s[1]=='X')) {
        s += 2;
    }
    long result = 0;
    const char *start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9')      digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }
    if (end) *end = (s == start) ? (char *)start : (char *)s;
    return result * sign;
}

unsigned long str_to_ulong(const char *s, char **end, int base) {
    return (unsigned long)str_to_long(s, end, base);
}

double str_to_double(const char *s, char **end) {
    while (*s == ' ') s++;
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') s++;
    double result = 0.0;
    while (*s >= '0' && *s <= '9') { result = result * 10.0 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') { result += (*s - '0') * frac; frac *= 0.1; s++; }
    }
    if (end) *end = (char *)s;
    return result * sign;
}


int vsprintf(char *str, const char *format, va_list args) {
    char *ptr = str;
    char temp_buf[32];

    while (*format) {
        if (*format != '%') {
            *ptr++ = *format++;
            continue;
        }
        format++;

        bool left_align = false;
        bool zero_pad   = false;
        bool plus_sign  = false;
        bool space_sign = false;

        bool parsing_flags = true;
        while (parsing_flags) {
            switch (*format) {
                case '-': left_align = true;  format++; break;
                case '0': zero_pad   = true;  format++; break;
                case '+': plus_sign  = true;  format++; break;
                case ' ': space_sign = true;  format++; break;
                default:  parsing_flags = false;        break;
            }
        }

        int width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }

        int precision = -1;
        if (*format == '.') {
            format++;
            precision = 0;
            while (*format >= '0' && *format <= '9') {
                precision = precision * 10 + (*format - '0');
                format++;
            }
        }

        if (*format == 'l' || *format == 'h') format++;

        #define WRITE_PADDED(s, slen)                              \
        do {                                                        \
            int _len = (slen);                                      \
            int _pad = (width > _len) ? width - _len : 0;          \
            char _pc = (zero_pad && !left_align) ? '0' : ' ';      \
            if (!left_align)                                        \
                for (int _i = 0; _i < _pad; _i++) *ptr++ = _pc;   \
            for (int _i = 0; _i < _len; _i++) *ptr++ = (s)[_i];   \
            if (left_align)                                         \
                for (int _i = 0; _i < _pad; _i++) *ptr++ = ' ';   \
        } while (0)

        switch (*format) {
            case 'c': {
                char c = (char)va_arg(args, int);
                char buf[1]; buf[0] = c;
                WRITE_PADDED(buf, 1);
                break;
            }

            case 's': {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                int slen = (int)strlen(s);
                if (precision >= 0 && slen > precision) slen = precision;
                WRITE_PADDED(s, slen);
                break;
            }

            case 'd':
            case 'i': {
                int val = va_arg(args, int);
                char tmp[24];
                int  ti   = 0;
                bool neg  = (val < 0);
                uint32_t uval = neg ? (uint32_t)(-(int32_t)val) : (uint32_t)val;

                if (uval == 0) {
                    tmp[ti++] = '0';
                } else {
                    while (uval > 0) { tmp[ti++] = '0' + (uval % 10); uval /= 10; }
                }

                if (precision > ti) {
                    while (ti < precision && ti < (int)sizeof(tmp) - 2)
                        tmp[ti++] = '0';
                }

                for (int a = 0, b = ti - 1; a < b; a++, b--) {
                    char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c;
                }

                char prefix[2]; int plen = 0;
                if (neg)             prefix[plen++] = '-';
                else if (plus_sign)  prefix[plen++] = '+';
                else if (space_sign) prefix[plen++] = ' ';

                int total = plen + ti;
                int pad   = (width > total) ? width - total : 0;
                char pc   = (zero_pad && !left_align) ? '0' : ' ';

                if (!left_align && !zero_pad)
                    for (int i = 0; i < pad; i++) *ptr++ = ' ';
                for (int i = 0; i < plen; i++) *ptr++ = prefix[i];
                if (!left_align && zero_pad)
                    for (int i = 0; i < pad; i++) *ptr++ = pc;
                for (int i = 0; i < ti; i++) *ptr++ = tmp[i];
                if (left_align)
                    for (int i = 0; i < pad; i++) *ptr++ = ' ';
                break;
            }

            case 'u': {
                uint32_t val = va_arg(args, uint32_t);
                char tmp[24];
                int  ti = 0;

                if (val == 0) {
                    tmp[ti++] = '0';
                } else {
                    while (val > 0) { tmp[ti++] = '0' + (val % 10); val /= 10; }
                }

                if (precision > ti)
                    while (ti < precision && ti < (int)sizeof(tmp) - 1)
                        tmp[ti++] = '0';

                for (int a = 0, b = ti - 1; a < b; a++, b--) {
                    char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c;
                }

                WRITE_PADDED(tmp, ti);
                break;
            }

            case 'x':
            case 'X': {
                uint32_t val = va_arg(args, uint32_t);
                const char *digits = (*format == 'X')
                                     ? "0123456789ABCDEF"
                                     : "0123456789abcdef";
                char tmp[24];
                int  ti = 0;

                if (val == 0) {
                    tmp[ti++] = '0';
                } else {
                    while (val > 0) { tmp[ti++] = digits[val % 16]; val /= 16; }
                }

                if (precision > ti)
                    while (ti < precision && ti < (int)sizeof(tmp) - 1)
                        tmp[ti++] = '0';

                for (int a = 0, b = ti - 1; a < b; a++, b--) {
                    char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c;
                }

                WRITE_PADDED(tmp, ti);
                break;
            }

            case 'o': {
                uint32_t val = va_arg(args, uint32_t);
                char tmp[24];
                int  ti = 0;
                if (val == 0) { tmp[ti++] = '0'; }
                else { while (val > 0) { tmp[ti++] = '0' + (val % 8); val /= 8; } }
                for (int a = 0, b = ti-1; a < b; a++, b--) {
                    char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c;
                }
                WRITE_PADDED(tmp, ti);
                break;
            }

            case 'p': {
                uint32_t val = (uint32_t)(uintptr_t)va_arg(args, void *);
                char tmp[10];
                tmp[0] = '0'; tmp[1] = 'x';
                const char *hd = "0123456789abcdef";
                for (int i = 0; i < 8; i++)
                    tmp[2 + i] = hd[(val >> (28 - i * 4)) & 0xF];
                WRITE_PADDED(tmp, 10);
                break;
            }

            case '%': {
                *ptr++ = '%';
                break;
            }

            default: {
                *ptr++ = '%';
                *ptr++ = *format;
                break;
            }
        }

        #undef WRITE_PADDED
        format++;
    }

    *ptr = '\0';
    return (int)(ptr - str);
}

int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsprintf(str, format, args);
    va_end(args);
    return written;
}

char *strdup(const char *s) {
    if (!s) return NULL;
    int len = (int)strlen(s) + 1;
    char *p = (char *)kmalloc(len);
    if (p) memcpy(p, s, len);
    return p;
}
char *strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len > n) len = n;
    char *p = (char *)kmalloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}
int  strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int  strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++; n--;
    }
    if (!n) return 0;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
size_t strlcpy(char *dst, const char *src, size_t dstsz) {
    if (!dstsz) return strlen(src);
    size_t i = 0;
    while (i < dstsz - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    while (src[i]) i++;
    return i;
}
size_t strlcat(char *dst, const char *src, size_t dstsz) {
    size_t dlen = strlen(dst);
    if (dlen >= dstsz) return dstsz + strlen(src);
    return dlen + strlcpy(dst + dlen, src, dstsz - dlen);
}
char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (str) *saveptr = str;
    if (!*saveptr) return NULL;

    while (**saveptr && strchr(delim, **saveptr)) (*saveptr)++;
    if (!**saveptr) { *saveptr = NULL; return NULL; }

    char *tok = *saveptr;
    while (**saveptr && !strchr(delim, **saveptr)) (*saveptr)++;

    if (**saveptr) {
        **saveptr = '\0';
        (*saveptr)++;
    } else {
        *saveptr = NULL;
    }
    return tok;
}

int isalpha(int c)  { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
int isdigit(int c)  { return c>='0'&&c<='9'; }
int isspace(int c)  { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
int isupper(int c)  { return c>='A'&&c<='Z'; }
int islower(int c)  { return c>='a'&&c<='z'; }
int isalnum(int c)  { return isalpha(c)||isdigit(c); }
int isprint(int c)  { return c>=32&&c<127; }
int ispunct(int c)  { return isprint(c)&&!isalnum(c)&&c!=' '; }
int isxdigit(int c) { return isdigit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
int iscntrl(int c)  { return c<32||c==127; }
int toupper(int c)  { return islower(c)?c-32:c; }
int tolower(int c)  { return isupper(c)?c+32:c; }


void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    uint8_t        b = (uint8_t)c;
    while (n--) {
        if (*p == b) return (void *)p;
        p++;
    }
    return NULL;
}
void *memmem(const void *haystack, size_t hlen,
             const void *needle,   size_t nlen) {
    if (nlen == 0) return (void *)haystack;
    if (hlen < nlen) return NULL;
    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, nlen) == 0)
            return (void *)(h + i);
    }
    return NULL;
}

const char *path_basename(const char *path) {
    if (!path || !*path) return path;
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}
void path_dirname(const char *path, char *dir, size_t dir_sz) {
    if (!path || !dir || dir_sz == 0) return;
    const char *last = strrchr(path, '/');
    if (!last || last == path) {
        strlcpy(dir, "/", dir_sz);
    } else {
        size_t len = (size_t)(last - path);
        if (len >= dir_sz) len = dir_sz - 1;
        memcpy(dir, path, len);
        dir[len] = '\0';
    }
}
void path_join(const char *base, const char *rel, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    strlcpy(out, base, out_sz);
    size_t blen = strlen(out);
    if (blen > 0 && out[blen - 1] != '/' && rel && rel[0] != '/') {
        strlcat(out, "/", out_sz);
    }
    if (rel) strlcat(out, rel, out_sz);
}
const char *path_ext(const char *path) {
    const char *base = path_basename(path);
    const char *dot  = strrchr(base, '.');
    return (dot && dot != base) ? dot + 1 : "";
}
