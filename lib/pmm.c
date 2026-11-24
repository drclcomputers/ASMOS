// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "lib/pmm.h"
#include "lib/memory.h"

#define BITMAP_WORDS ((PMM_MAX_FRAMES + 31) / 32)

static uint32_t s_bitmap[BITMAP_WORDS];
static uint32_t s_base;
static uint32_t s_nframes;

static inline void pmm_set(uint32_t i) { s_bitmap[i >> 5] |= 1u << (i & 31); }
static inline void pmm_clr(uint32_t i) {
    s_bitmap[i >> 5] &= ~(1u << (i & 31));
}
static inline bool pmm_tst(uint32_t i) {
    return (s_bitmap[i >> 5] >> (i & 31)) & 1u;
}

bool pmm_init(uint32_t base, uint32_t frames) {
    if (base & (PMM_PAGE_SIZE - 1))
        return false;
    if (frames > PMM_MAX_FRAMES)
        frames = PMM_MAX_FRAMES;
    memset(s_bitmap, 0, sizeof(s_bitmap));
    s_base = base;
    s_nframes = frames;
    return frames > 0;
}

uint32_t pmm_alloc_frames(uint32_t n) {
    if (n == 0 || n > s_nframes)
        return 0;
    uint32_t run = 0;
    for (uint32_t i = 0; i < s_nframes; i++) {
        run = pmm_tst(i) ? 0 : run + 1;
        if (run == n) {
            uint32_t first = i + 1 - n;
            for (uint32_t j = first; j <= i; j++)
                pmm_set(j);
            uint32_t addr = s_base + first * PMM_PAGE_SIZE;
            memset((void *)(uintptr_t)addr, 0, n * PMM_PAGE_SIZE);
            return addr;
        }
    }
    return 0;
}

uint32_t pmm_alloc_frame(void) { return pmm_alloc_frames(1); }

void pmm_free_frame(uint32_t addr) {
    if (!pmm_owns(addr) || (addr & (PMM_PAGE_SIZE - 1)))
        return;
    pmm_clr((addr - s_base) / PMM_PAGE_SIZE);
}

void pmm_free_frames(uint32_t addr, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        pmm_free_frame(addr + i * PMM_PAGE_SIZE);
}

uint32_t pmm_free_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_nframes; i++)
        n += !pmm_tst(i);
    return n;
}

uint32_t pmm_total(void) { return s_nframes; }
uint32_t pmm_base(void) { return s_base; }
uint32_t pmm_end(void) { return s_base + s_nframes * PMM_PAGE_SIZE; }

bool pmm_owns(uint32_t addr) {
    return s_nframes && addr >= s_base && addr < pmm_end();
}
