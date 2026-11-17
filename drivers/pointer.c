#include "drivers/pointer.h"
#include "drivers/gpu.h"

static const uint8_t *s_pix;
static int s_w, s_h, s_hx, s_hy;
static int s_x, s_y;
static bool s_on;
/* what is on the screen now */
static bool s_drawn;
static const uint8_t *s_dpix;
static int s_dx, s_dy, s_dw, s_dh;

/* the old place back from the last frame, the pointer at the new one.
 * A short move is sent as one rectangle (old and new together): nothing
 * flickers and planar VGA writes each 32-pixel block once. */
static uint8_t s_tmp[40 * 40];

static void redraw(void) {
    int x = s_x - s_hx, y = s_y - s_hy;
    bool want = s_on && s_pix;
    if (s_drawn && want && s_pix == s_dpix && x == s_dx && y == s_dy)
        return; /* already there */
    if (s_drawn && want) {
        int ux = x < s_dx ? x : s_dx, uy = y < s_dy ? y : s_dy;
        int ux1 = x + s_w > s_dx + s_dw ? x + s_w : s_dx + s_dw;
        int uy1 = y + s_h > s_dy + s_dh ? y + s_h : s_dy + s_dh;
        int uw = ux1 - ux, uh = uy1 - uy;
        if (uw <= 40 && uh <= 40) {
            for (int i = 0; i < uw * uh; i++)
                s_tmp[i] = 0xFF;
            for (int r = 0; r < s_h; r++)
                for (int c = 0; c < s_w; c++)
                    s_tmp[(y - uy + r) * uw + (x - ux + c)] =
                        s_pix[r * s_w + c];
            gpu_put(ux, uy, uw, uh, s_tmp);
            s_dpix = s_pix;
            s_dx = x, s_dy = y, s_dw = s_w, s_dh = s_h;
            return;
        }
    }
    if (s_drawn) {
        gpu_put(s_dx, s_dy, s_dw, s_dh, NULL);
        s_drawn = false;
    }
    if (!want)
        return;
    gpu_put(x, y, s_w, s_h, s_pix);
    s_drawn = true;
    s_dpix = s_pix;
    s_dx = x, s_dy = y, s_dw = s_w, s_dh = s_h;
}

void pointer_shape(const uint8_t *pix, int w, int h, int hx, int hy) {
    uint32_t f = gpu_lock();
    s_pix = pix;
    s_w = w, s_h = h, s_hx = hx, s_hy = hy;
    redraw();
    gpu_unlock(f);
}

void pointer_move(int x, int y) {
    uint32_t f = gpu_lock();
    s_x = x, s_y = y;
    redraw();
    gpu_unlock(f);
}

void pointer_show(bool on) {
    uint32_t f = gpu_lock();
    s_on = on;
    redraw();
    gpu_unlock(f);
}

bool pointer_visible(void) { return s_on && s_drawn; }

void pointer_forget(void) {
    uint32_t f = gpu_lock();
    s_on = false;
    s_drawn = false;
    gpu_unlock(f);
}

/* the blit wrote these rows from the back buffer, over the pointer */
void pointer_after_blit(int y0, int y1) {
    uint32_t f = gpu_lock();
    if (s_drawn && s_dy <= y1 && s_dy + s_dh > y0)
        gpu_put(s_dx, s_dy, s_dw, s_dh, s_dpix);
    gpu_unlock(f);
}
