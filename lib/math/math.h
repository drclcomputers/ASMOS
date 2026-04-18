#ifndef MATH_H
#define MATH_H

#include "lib/core.h"

static inline int abs(int x) { return (x < 0) ? -x : x; }

static inline int max(int a, int b) { return (a > b) ? a : b; }

static inline int min(int a, int b) { return (a < b) ? a : b; }

static inline int clamp(int x, int lo, int hi) {
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

static inline uint32_t clamp_u(uint32_t x, uint32_t lo, uint32_t hi) {
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

static inline int lerp_i(int a, int b, int t_256) {
    return a + ((b - a) * t_256) / 256;
}

static inline int sign(int x) { return (x > 0) - (x < 0); }

static inline bool is_pow2(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

static inline double fabs(double x) { return x < 0 ? -x : x; }
static inline double floor(double x) { return (double)(int)x; }
static inline double ceil(double x) {
    int i = (int)x;
    return (x > (double)i) ? (double)(i + 1) : (double)i;
}

static inline uint32_t div_ceil(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

static inline uint32_t align_up(uint32_t x, uint32_t align) {
    return (x + align - 1) & ~(align - 1);
}

static inline uint32_t align_down(uint32_t x, uint32_t align) {
    return x & ~(align - 1);
}

static inline int map_range(int v, int in_min, int in_max, int out_min,
                            int out_max) {
    if (in_max == in_min)
        return out_min;
    return out_min + (v - in_min) * (out_max - out_min) / (in_max - in_min);
}

static inline int wrap(int x, int period) {
    int r = x % period;
    return (r < 0) ? r + period : r;
}

static inline uint32_t abs_diff(int a, int b) {
    return (uint32_t)((a > b) ? (a - b) : (b - a));
}

uint32_t isqrt(uint32_t n);
int32_t ipow(int32_t base, uint32_t exp);
uint32_t next_pow2(uint32_t n);
uint32_t log2_floor(uint32_t n);

void rand_seed(uint32_t seed);
uint32_t rand_next(void);

static inline int rand_range(int lo, int hi) {
    if (hi <= lo)
        return lo;
    return lo + (int)(rand_next() % (uint32_t)(hi - lo + 1));
}

#endif
