#ifndef ALLOC_H
#define ALLOC_H

#include "config/config.h"
#include "lib/core.h"

#define HEAP_MAGIC 0xDEADBEEF
#define BLOCK_MAGIC_FREE 0xF4EEB10C
#define BLOCK_MAGIC_USED 0xA110CA7E

/* A heap: a block list inside [start, end). */
typedef struct heap {
    void *first;
    uint32_t start, end;
} heap_t;

bool heap_init(heap_t *h, uint32_t start, uint32_t end);
void *heap_alloc(heap_t *h, uint32_t size);
void heap_free(heap_t *h, void *p); /* ignores pointers it doesn't own */
bool heap_owns(const heap_t *h, const void *p);
uint32_t heap_used_in(const heap_t *h);
uint32_t heap_free_in(const heap_t *h);
bool heap_check(const heap_t *h); /* false if a block header is broken */

extern heap_t g_kheap; /* the kernel heap behind kmalloc/kfree */
/* Second kernel heap: the free memory below 640 KB between the kernel and
 * its stack. kmalloc uses it when the main heap is full. */
extern heap_t g_kheap_low;
void alloc_add_low(uint32_t start, uint32_t end);

void alloc_set_range(uint32_t start, uint32_t end);
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
