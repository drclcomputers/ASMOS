// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

/* Pictures: decoded by the OS (lib/image.h: GIF, JPEG, BMP), to the
 * screen's colours; and the screen's nearest colour to one a page asks
 * for (<font color>, <body bgcolor>). */

#include "extapps/worldview/wv.h"

static uint8_t s_pal[256][3]; /* the screen's, 0..63 */
static int s_ncol = 16;
static uint8_t s_lut[4096]; /* 4 bits of each: the nearest, 0xFF unknown */

void img_palette_init(void) {
    int n = A->screen_palette(s_pal, 256);
    s_ncol = n > APP_COLOR_CUSTOM ? APP_COLOR_CUSTOM : n < 16 ? 16 : n;
    memset(s_lut, 0xFF, sizeof(s_lut));
}

uint8_t img_nearest(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    int i = (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4);
    if (s_lut[i] == 0xFF) {
        int best = 0, bd = 0x7FFFFFFF;
        for (int k = 0; k < s_ncol; k++) {
            int dr = r - s_pal[k][0] * 4, dg = g - s_pal[k][1] * 4,
                db = b - s_pal[k][2] * 4;
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        s_lut[i] = (uint8_t)best;
    }
    return s_lut[i];
}

int img_kind(const uint8_t *b, uint32_t n) { return image_kind(b, n); }

bool img_size(const uint8_t *b, uint32_t n, int *w, int *h) {
    return image_info(b, n, w, h) == NULL;
}

uint8_t *img_decode(const uint8_t *b, uint32_t n, int dw, int dh, uint8_t bg,
                    const char **why) {
    *why = NULL;
    if (dw < 1 || dh < 1 || (uint32_t)(dw * dh) > WV_IMG_PIX_MAX) {
        *why = "too big";
        return NULL;
    }
    uint8_t *out = (uint8_t *)kmalloc((uint32_t)(dw * dh));
    if (!out) {
        *why = "not enough memory";
        return NULL;
    }
    image_target_t t = {(const uint8_t (*)[3])s_pal,
                        s_ncol,
                        true,
                        {(uint8_t)(s_pal[bg][0] * 4),
                         (uint8_t)(s_pal[bg][1] * 4),
                         (uint8_t)(s_pal[bg][2] * 4)},
                        NULL,
                        NULL};
    *why = image_decode(b, n, out, dw, dh, dw, &t);
    if (*why) {
        kfree(out);
        return NULL;
    }
    return out;
}
