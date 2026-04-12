#ifndef BITOPS_H
#define BITOPS_H

#include "lib/core.h"

static inline bool bit_test(uint32_t val, uint8_t bit) {
    return (val >> bit) & 1;
}

static inline uint32_t bit_set(uint32_t val, uint8_t bit) {
    return val | (1u << bit);
}

static inline uint32_t bit_clear(uint32_t val, uint8_t bit) {
    return val & ~(1u << bit);
}

static inline uint32_t bit_toggle(uint32_t val, uint8_t bit) {
    return val ^ (1u << bit);
}

static inline uint32_t bit_write(uint32_t val, uint8_t bit, bool cond) {
    return cond ? bit_set(val, bit) : bit_clear(val, bit);
}

static inline uint32_t bits_get(uint32_t val, uint8_t lo, uint8_t width) {
    return (val >> lo) & ((1u << width) - 1);
}

static inline uint32_t bits_set(uint32_t val, uint8_t lo, uint8_t width, uint32_t field) {
    uint32_t mask = ((1u << width) - 1) << lo;
    return (val & ~mask) | ((field << lo) & mask);
}

static inline uint32_t popcount32(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (v * 0x01010101u) >> 24;
}

static inline uint32_t ctz32(uint32_t v) {
    if (v == 0) return 32;
    uint32_t n = 0;
    if (!(v & 0x0000FFFFu)) { n += 16; v >>= 16; }
    if (!(v & 0x000000FFu)) { n +=  8; v >>=  8; }
    if (!(v & 0x0000000Fu)) { n +=  4; v >>=  4; }
    if (!(v & 0x00000003u)) { n +=  2; v >>=  2; }
    if (!(v & 0x00000001u)) { n +=  1; }
    return n;
}

static inline uint32_t clz32(uint32_t v) {
    if (v == 0) return 32;
    uint32_t n = 0;
    if (!(v & 0xFFFF0000u)) { n += 16; v <<= 16; }
    if (!(v & 0xFF000000u)) { n +=  8; v <<=  8; }
    if (!(v & 0xF0000000u)) { n +=  4; v <<=  4; }
    if (!(v & 0xC0000000u)) { n +=  2; v <<=  2; }
    if (!(v & 0x80000000u)) { n +=  1; }
    return n;
}

static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0xFF000000u) >> 24);
}

static inline uint32_t rotl32(uint32_t v, uint8_t n) {
    return (v << n) | (v >> (32 - n));
}

static inline uint32_t rotr32(uint32_t v, uint8_t n) {
    return (v >> n) | (v << (32 - n));
}

static inline uint32_t bitmask(uint8_t lo, uint8_t width) {
    return ((1u << width) - 1u) << lo;
}

static inline uint64_t bswap64(uint64_t v) {
    return ((uint64_t)bswap32((uint32_t)(v & 0xFFFFFFFFu)) << 32) |
            (uint64_t)bswap32((uint32_t)(v >> 32));
}

static inline uint64_t rotl64(uint64_t v, uint8_t n) {
    return (v << n) | (v >> (64 - n));
}

#endif
