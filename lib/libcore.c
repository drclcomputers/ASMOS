#include "libcore.h"
#include "math.h"
#include "types.h"

void exit(int code) {
    (void)code;
    os_request_exit();
    for (;;) __asm__ volatile ("hlt");
}

void abort(void) {
    fprintf(stderr, "abort() called\n");
    exit(1);
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    uint8_t *arr = (uint8_t *)base;
    uint8_t *tmp = (uint8_t *)kmalloc(size);
    if (!tmp) return;
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, arr + i * size, size);
        int j = (int)i - 1;
        while (j >= 0 && cmp(arr + j * size, tmp) > 0) {
            memcpy(arr + (j + 1) * size, arr + j * size, size);
            j--;
        }
        memcpy(arr + (j + 1) * size, tmp, size);
    }
    kfree(tmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    const uint8_t *arr = (const uint8_t *)base;
    int lo = 0, hi = (int)n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int r = cmp(key, arr + mid * size);
        if (r == 0) return (void *)(arr + mid * size);
        if (r < 0)  hi = mid - 1;
        else        lo = mid + 1;
    }
    return NULL;
}
