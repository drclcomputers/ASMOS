#include "lib/string.h"
#include "lib/types.h"

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

// Format text with args on the go
int vsprintf(char *str, const char *format, va_list args) {
    char *ptr = str;
    char temp_buf[32];

    while (*format) {
        if (*format != '%') {
            *ptr++ = *format++;
            continue;
        }

        format++;

        switch (*format) {
            case 'c': {
                char c = (char)va_arg(args, int);
                *ptr++ = c;
                break;
            }
            case 's': {
                char *s = va_arg(args, char*);
                if (!s) s = "(null)";
                while (*s) *ptr++ = *s++;
                break;
            }
            case 'd':
            case 'i': {
                int val = va_arg(args, int);
                int_to_str(val, temp_buf);
                char *t = temp_buf;
                while (*t) *ptr++ = *t++;
                break;
            }
            case 'u': {
                uint32_t val = va_arg(args, uint32_t);
                uint32_to_str(val, temp_buf);
                char *t = temp_buf;
                while (*t) *ptr++ = *t++;
                break;
            }
            case 'x':
            case 'X': {
                uint32_t val = va_arg(args, uint32_t);
                uint32_to_hex_str(val, temp_buf, (*format == 'X'));
                char *t = temp_buf;
                while (*t) *ptr++ = *t++;
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
        format++;
    }
    *ptr = '\0';
    return ptr - str;
}

int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsprintf(str, format, args);
    va_end(args);
    return written;
}
