#include "lib/math/math.h"

static uint32_t s_rand_state = 12345;

void rand_seed(uint32_t seed) {
    s_rand_state = seed ? seed : 1;
}

uint32_t rand_next(void) {
    uint32_t x = s_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rand_state = x;
    return x;
}

uint32_t isqrt(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n;
    uint32_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

int32_t ipow(int32_t base, uint32_t exp) {
    int32_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return result;
}

uint32_t next_pow2(uint32_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

uint32_t log2_floor(uint32_t n) {
    uint32_t r = 0;
    while (n >>= 1) r++;
    return r;
}
