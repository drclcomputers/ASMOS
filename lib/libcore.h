#ifndef LIBCORE_H
#define LIBCORE_H

void exit(int code);
void abort(void);
char *getenv(const char *name);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

#endif
