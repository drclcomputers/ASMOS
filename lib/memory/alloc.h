#ifndef ALLOC_H
#define ALLOC_H

#include "config/config.h"
#include "lib/core.h"

void alloc_init(void);
void alloc_set_end(uint32_t end);

void *kmalloc(uint32_t size);
void *kzalloc(uint32_t size);
void *krealloc(void *ptr, uint32_t new_size);
void *kmalloc_aligned(uint32_t size, uint32_t align);
void kfree(void *ptr);

uint32_t heap_used(void);
uint32_t heap_remaining(void);

#define POOL_STORAGE_SIZE(capacity, block_sz)                                  \
    ((capacity) * (((block_sz) + 3) & ~3u))

typedef struct {
    uint8_t *slab;
    uint32_t *free_stack;
    uint32_t block_size;
    uint32_t capacity;
    uint32_t free_count;
} pool_t;

bool pool_init(pool_t *pool, void *slab, uint32_t capacity,
               uint32_t block_size);

void pool_destroy(pool_t *pool);

void *pool_alloc(pool_t *pool);

void pool_free(pool_t *pool, void *ptr);

static inline bool pool_empty(const pool_t *pool) {
    return pool->free_count == 0;
}
static inline uint32_t pool_available(const pool_t *pool) {
    return pool->free_count;
}

#endif
