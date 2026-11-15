#ifndef GPU_H
#define GPU_H

#include "lib/core.h"

typedef enum {
    GPU_BACKEND_NONE,
    GPU_BACKEND_MODEX,
} gpu_backend_t;

void gpu_init(void);
void gpu_blit(void);            /* sends only what changed since last frame */
void gpu_invalidate(void);      /* next blit sends the whole screen */
uint32_t gpu_fps(void);         /* frames per second, updated each second */
uint32_t gpu_last_blocks(void); /* 32-pixel blocks the last blit sent */
gpu_backend_t gpu_backend(void);

#endif
