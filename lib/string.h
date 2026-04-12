#ifndef STRING_H
#define STRING_H

#include "lib/types.h"

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, l)   __builtin_va_arg(v, l)

size_t strlen(const char* s);
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
char* strcat(char* dst, const char* src);
char* strncat(char* dst, const char* src, size_t n);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);

char* uint32_to_str(uint32_t num, char* str);
uint32_t str_to_uint32(const char* str);
int str_to_int(const char* str);
char* int_to_str(int num, char* str);
void uint32_to_hex_str(uint32_t num, char* str, bool uppercase);
long str_to_long(const char *s, char **end, int base);
unsigned long str_to_ulong(const char *s, char **end, int base);
double str_to_double(const char *s, char **end);

int vsprintf(char *str, const char *format, va_list args);
int sprintf(char *str, const char *format, ...);

char *strdup(const char *s);
char *strndup(const char *s, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

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

#endif
