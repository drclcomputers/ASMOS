#include "lib/pmm.h"
#include "lib/memory.h"

#define BITMAP_WORDS ((PMM_MAX_FRAMES + 31) / 32)

static uint32_t s_bitmap[BITMAP_WORDS];
static uint8_t *s_pool = NULL;
static uint32_t s_nframes = 0;

static inline void pmm_bit_set(uint32_t idx) {
    s_bitmap[idx >> 5] |= (1u << (idx & 31));
}
static inline void pmm_bit_clr(uint32_t idx) {
    s_bitmap[idx >> 5] &= ~(1u << (idx & 31));
}
static inline bool pmm_bit_tst(uint32_t idx) {
    return (s_bitmap[idx >> 5] >> (idx & 31)) & 1u;
}

bool pmm_init(void) {
    uint32_t pool_bytes = PMM_MAX_FRAMES * PMM_PAGE_SIZE;
    s_pool = (uint8_t *)kmalloc_aligned(pool_bytes, PMM_PAGE_SIZE);
    if (!s_pool)
        return false;

    memset(s_pool, 0, pool_bytes);

    memset(s_bitmap, 0, sizeof(s_bitmap));
    s_nframes = PMM_MAX_FRAMES;

    return true;
}

uint32_t pmm_alloc_frame(void) {
    for (uint32_t w = 0; w < BITMAP_WORDS; w++) {
        if (s_bitmap[w] == 0xFFFFFFFFu)
            continue;
        for (uint32_t b = 0; b < 32; b++) {
            uint32_t idx = (w << 5) | b;
            if (idx >= s_nframes)
                return 0;
            if (!pmm_bit_tst(idx)) {
                pmm_bit_set(idx);
                return (uint32_t)(uintptr_t)(s_pool + idx * PMM_PAGE_SIZE);
            }
        }
    }
    return 0;
}

void pmm_free_frame(uint32_t phys_addr) {
    if (!s_pool || phys_addr == 0)
        return;
    uint32_t pool_base = (uint32_t)(uintptr_t)s_pool;
    if (phys_addr < pool_base)
        return;
    uint32_t offset = phys_addr - pool_base;
    if (offset & (PMM_PAGE_SIZE - 1))
        return;
    uint32_t idx = offset / PMM_PAGE_SIZE;
    if (idx >= s_nframes)
        return;
    memset((void *)(uintptr_t)phys_addr, 0, PMM_PAGE_SIZE);
    pmm_bit_clr(idx);
}

uint32_t pmm_alloc_frames(uint32_t n) {
    if (n == 0 || n > s_nframes)
        return 0;
    uint32_t run_start = 0, run_len = 0;
    for (uint32_t i = 0; i < s_nframes; i++) {
        if (!pmm_bit_tst(i))
            if (run_len == 0)
                run_start = i;
        run_len++;
        if (run_len == n) {
            for (uint32_t j = run_start; j < run_start + n; j++)
                pmm_bit_set(j);
            return (uint32_t)(uintptr_t)(s_pool + run_start * PMM_PAGE_SIZE);
        } else {
            run_len = 0;
        }
    }
    return 0;
}

uint32_t pmm_free_count(void) {
    uint32_t free = 0;
    for (uint32_t i = 0; i < s_nframes; i++)
        if (!pmm_bit_tst(i))
            free++;
    return free;
}

void pmm_free_frames(uint32_t phys_addr, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        pmm_free_frame(phys_addr + i * PMM_PAGE_SIZE);
}
