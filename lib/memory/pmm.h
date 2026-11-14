#ifndef PMM_H
#define PMM_H

#include "lib/core.h"

#define PMM_PAGE_SIZE  4096u
#define PMM_MAX_FRAMES 1024          // 4MB of physical page pool

bool pmm_init(void);

uint32_t pmm_alloc_frame(void);
void pmm_free_frame(uint32_t phys_addr);
uint32_t pmm_alloc_frames(uint32_t n);
uint32_t pmm_free_count(void);
void pmm_free_frames(uint32_t phys_addr, uint32_t n);

#endif // PMM_H
