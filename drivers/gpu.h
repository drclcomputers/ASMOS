#ifndef GPU_H
#define GPU_H

#include "lib/core.h"

typedef enum {
    GPU_BACKEND_NONE,
    GPU_BACKEND_MODEX,
} gpu_backend_t;

void gpu_init(void);
void gpu_blit(void); /* sends only what changed since last frame */
void gpu_blit_rows(int y0, int y1); /* ... when only these rows were drawn */
void gpu_blit_skip(void);       /* nothing was drawn: just count the frame */
void gpu_invalidate(void);      /* next blit sends the whole screen */
uint32_t gpu_fps(void);         /* frames per second, updated each second */
uint32_t gpu_last_blocks(void); /* 32-pixel blocks the last blit sent */
gpu_backend_t gpu_backend(void);

/* w*h pixels (0xFF = see-through; NULL = none) over the last frame sent,
 * straight onto the screen; the back buffer is not touched. Safe from an
 * interrupt (VGA registers are saved). Used for the pointer
 * (drivers/pointer.h). */
void gpu_put(int x, int y, int w, int h, const uint8_t *pix);
/* interrupts off / back as they were, around screen work the timer
 * interrupt must not cut into */
uint32_t gpu_lock(void);
void gpu_unlock(uint32_t flags);

#endif
