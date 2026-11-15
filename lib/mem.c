#include "lib/mem.h"
#include "lib/core.h"

/* String instructions: on a 386/486 `rep movsd` / `rep stosd` move 4 bytes
 * in a few clocks, where a byte loop costs ~7 clocks a byte. Everything
 * (fill_rect, window paints, the blit's shadow copy) goes through these. */

void *memset(void *dst, int c, size_t n) {
    uint32_t v = (uint8_t)c;
    v |= v << 8;
    v |= v << 16;
    void *d = dst;
    size_t head = (size_t)(-(uintptr_t)d) & 3; /* align the dwords */
    if (head > n)
        head = n;
    n -= head;
    size_t dw = n >> 2, tail = n & 3;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(head) : "a"(v) : "memory");
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(dw) : "a"(v) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(tail) : "a"(v) : "memory");
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    void *d = dst;
    const void *s = src;
    size_t head = (size_t)(-(uintptr_t)d) & 3; /* align the writes */
    if (head > n)
        head = n;
    n -= head;
    size_t dw = n >> 2, tail = n & 3;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(head) : : "memory");
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(dw) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(tail) : : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n); /* forward copy is safe */
    /* overlapping, dst above src: copy backwards */
    d += n - 1;
    s += n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld"
                     : "+D"(d), "+S"(s), "+c"(n)
                     :
                     : "memory");
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q)
            return *p - *q;
        p++;
        q++;
    }
    return 0;
}
