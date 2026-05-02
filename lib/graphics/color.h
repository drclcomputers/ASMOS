#ifndef COLOR_H
#define COLOR_H

#include "config/config.h"
#include "lib/core.h"

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t PALETTE_RGB[16] = {
    [BLACK] = {0, 0, 0},           [BLUE] = {0, 0, 170},
    [GREEN] = {0, 170, 0},         [CYAN] = {0, 170, 170},
    [RED] = {170, 0, 0},           [MAGENTA] = {170, 0, 170},
    [BROWN] = {170, 85, 0},        [LIGHT_GRAY] = {170, 170, 170},
    [DARK_GRAY] = {85, 85, 85},    [LIGHT_BLUE] = {85, 85, 255},
    [LIGHT_GREEN] = {85, 255, 85}, [LIGHT_CYAN] = {85, 255, 255},
    [LIGHT_RED] = {255, 85, 85},   [LIGHT_MAGENTA] = {255, 85, 255},
    [YELLOW] = {255, 255, 85},     [WHITE] = {255, 255, 255},
};

static inline uint8_t color_blend(uint8_t a, uint8_t b, uint8_t alpha) {
    if (alpha == 0)
        return a;
    if (alpha == 255)
        return b;

    rgb_t ca = PALETTE_RGB[a & 15];
    rgb_t cb = PALETTE_RGB[b & 15];

    uint8_t tr = (uint8_t)(ca.r + ((int)(cb.r - ca.r) * alpha) / 256);
    uint8_t tg = (uint8_t)(ca.g + ((int)(cb.g - ca.g) * alpha) / 256);
    uint8_t tb = (uint8_t)(ca.b + ((int)(cb.b - ca.b) * alpha) / 256);

    uint32_t best_dist = 0xFFFFFFFFu;
    uint8_t best_idx = a;
    for (int i = 0; i < 16; i++) {
        int dr = (int)PALETTE_RGB[i].r - tr;
        int dg = (int)PALETTE_RGB[i].g - tg;
        int db = (int)PALETTE_RGB[i].b - tb;
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = (uint8_t)i;
        }
    }
    return best_idx;
}

static inline uint8_t color_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t best_dist = 0xFFFFFFFFu;
    uint8_t best_idx = 0;
    for (int i = 0; i < 16; i++) {
        int dr = (int)PALETTE_RGB[i].r - r;
        int dg = (int)PALETTE_RGB[i].g - g;
        int db = (int)PALETTE_RGB[i].b - b;
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = (uint8_t)i;
        }
    }
    return best_idx;
}

static inline uint8_t color_lighten(uint8_t c) {
    switch (c) {
    case BLACK:
        return DARK_GRAY;
    case BLUE:
        return LIGHT_BLUE;
    case GREEN:
        return LIGHT_GREEN;
    case CYAN:
        return LIGHT_CYAN;
    case RED:
        return LIGHT_RED;
    case MAGENTA:
        return LIGHT_MAGENTA;
    case BROWN:
        return YELLOW;
    case LIGHT_GRAY:
        return WHITE;
    case DARK_GRAY:
        return LIGHT_GRAY;
    default:
        return c;
    }
}

static inline uint8_t color_darken(uint8_t c) {
    switch (c) {
    case WHITE:
        return LIGHT_GRAY;
    case LIGHT_GRAY:
        return DARK_GRAY;
    case DARK_GRAY:
        return BLACK;
    case LIGHT_BLUE:
        return BLUE;
    case LIGHT_GREEN:
        return GREEN;
    case LIGHT_CYAN:
        return CYAN;
    case LIGHT_RED:
        return RED;
    case LIGHT_MAGENTA:
        return MAGENTA;
    case YELLOW:
        return BROWN;
    default:
        return BLACK;
    }
}

static inline bool color_is_bright(uint8_t c) { return c >= DARK_GRAY; }

static inline uint8_t color_invert(uint8_t c) { return (uint8_t)(c ^ 8); }

static inline uint8_t color_contrast_fg(uint8_t bg) {
    rgb_t c = PALETTE_RGB[bg & 15];
    int lum = (c.r * 299 + c.g * 587 + c.b * 114) / 1000;
    return (lum > 127) ? BLACK : WHITE;
}

#endif
