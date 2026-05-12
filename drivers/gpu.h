#ifndef GPU_H
#define GPU_H

#include "lib/core.h"

typedef enum {
    GPU_BACKEND_NONE,
    GPU_BACKEND_MODEX,
} gpu_backend_t;

void gpu_init(void);
void gpu_blit(void);
gpu_backend_t gpu_backend(void);

#endif
