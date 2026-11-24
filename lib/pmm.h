// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef PMM_H
#define PMM_H

#include "lib/core.h"

/* Page-frame pool: a page-aligned region set aside at boot (top of the
 * heap range) for app images, app heaps and page tables. 4 KB frames. */

#define PMM_PAGE_SIZE 4096u
#define PMM_MAX_FRAMES 2048 /* 8 MB at most */

bool pmm_init(uint32_t base, uint32_t frames);

uint32_t pmm_alloc_frame(void);        /* 0 = none */
uint32_t pmm_alloc_frames(uint32_t n); /* contiguous; 0 = none */
void pmm_free_frame(uint32_t phys_addr);
void pmm_free_frames(uint32_t phys_addr, uint32_t n);
uint32_t pmm_free_count(void);
uint32_t pmm_total(void);
uint32_t pmm_base(void); /* region [pmm_base, pmm_end) */
uint32_t pmm_end(void);
bool pmm_owns(uint32_t addr);

#endif // PMM_H
