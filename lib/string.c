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
