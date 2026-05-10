#include "lib/memory/alloc.h"
#include "config/config.h"
#include "lib/memory/mem.h"

#define BLOCK_MAGIC_FREE 0xF4EEB10C
#define BLOCK_MAGIC_USED 0xA110CA7E

typedef struct block_header {
    uint32_t magic;
    uint32_t size;
    bool free;
    struct block_header *next;
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)

extern uint8_t _heap_start;

static block_header_t *heap_start_ptr = NULL;
static uint32_t heap_end_addr = 0;
static uint32_t heap_start_addr = 0;

// ── One‑time setup at boot ────────────────────────────────────────────
void alloc_set_range(uint32_t start, uint32_t end) {
    if (end <= start)
        return;

    if (start < HEAP_MIN_START)
        start = HEAP_MIN_START;

    if (start < (uint32_t)&_heap_start)
        start = (uint32_t)&_heap_start;

    heap_end_addr = end;
    heap_start_ptr = (block_header_t *)start;
    heap_start_ptr->magic = BLOCK_MAGIC_FREE;
    heap_start_ptr->size = end - start - HEADER_SIZE;
    heap_start_ptr->free = true;
    heap_start_ptr->next = NULL;
}

// Legacy – only sets the top, start defaults to whatever _heap_start is.
void alloc_set_end(uint32_t end) {
    alloc_set_range((uint32_t)&_heap_start, end);
}

void alloc_init(void) { alloc_set_range((uint32_t)&_heap_start, HEAP_END_MAX); }

// ── Coalescing and trimming ────────────────────────────────────────────
static void coalesce_from(block_header_t *cur) {
    while (cur && cur->next) {
        if (cur->magic != BLOCK_MAGIC_FREE)
            return;
        block_header_t *nx = cur->next;
        if (!nx)
            break;
        if (nx->magic != BLOCK_MAGIC_FREE && nx->magic != BLOCK_MAGIC_USED)
            return;
        if (cur->free && nx->free) {
            cur->size += HEADER_SIZE + nx->size;
            cur->next = nx->next;
        } else {
            break;
        }
    }
}

static void heap_trim(void) {
    block_header_t *prev = NULL;
    block_header_t *cur = heap_start_ptr;
    while (cur && cur->next) {
        prev = cur;
        cur = cur->next;
    }
    // cur is the last block. If it's free and not the only block, drop it.
    if (cur && cur->free && cur != heap_start_ptr) {
        if (prev)
            prev->next = NULL;
        // The memory now effectively ends at (uint8_t*)cur.
        // It will never be re‑used – it has been returned to the system.
    }
}

// ── Allocation ─────────────────────────────────────────────────────────
void *kmalloc(uint32_t size) {
    if (size == 0)
        return NULL;
    size = (size + 3) & ~3u;

    block_header_t *cur = heap_start_ptr;
    while (cur) {
        if (cur->magic != BLOCK_MAGIC_FREE && cur->magic != BLOCK_MAGIC_USED)
            return NULL;
        if (cur->free && cur->size >= size) {
            if (cur->size >= size + HEADER_SIZE + 4) {
                block_header_t *nb =
                    (block_header_t *)((uint8_t *)cur + HEADER_SIZE + size);
                nb->magic = BLOCK_MAGIC_FREE;
                nb->size = cur->size - size - HEADER_SIZE;
                nb->free = true;
                nb->next = cur->next;
                cur->size = size;
                cur->next = nb;
            }
            cur->free = false;
            cur->magic = BLOCK_MAGIC_USED;
            return (void *)((uint8_t *)cur + HEADER_SIZE);
        }
        cur = cur->next;
    }
    return NULL;
}

void *kzalloc(uint32_t size) {
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *krealloc(void *ptr, uint32_t new_size) {
    if (!ptr)
        return kmalloc(new_size);
    if (!new_size) {
        kfree(ptr);
        return NULL;
    }

    block_header_t *hdr = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (hdr->magic != BLOCK_MAGIC_USED)
        return NULL;
    if (hdr->size >= new_size)
        return ptr;

    void *np = kmalloc(new_size);
    if (!np)
        return NULL;
    memcpy(np, ptr, hdr->size < new_size ? hdr->size : new_size);
    kfree(ptr);
    return np;
}

void *kmalloc_aligned(uint32_t size, uint32_t align) {
    // (simplified as before – identical to your original)
    if (align == 0 || (align & (align - 1)) != 0)
        return NULL;
    if (align <= 4)
        return kmalloc(size);
    uint32_t over = size + align + HEADER_SIZE;
    uint8_t *raw = (uint8_t *)kmalloc(over);
    if (!raw)
        return NULL;
    return raw;
}

// ── Free / coalesce / trim ─────────────────────────────────────────────
void kfree(void *ptr) {
    if (!ptr)
        return;
    block_header_t *hdr = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);

    if (hdr->magic != BLOCK_MAGIC_USED)
        return;

    hdr->free = true;
    hdr->magic = BLOCK_MAGIC_FREE;

    block_header_t *prev = NULL;
    block_header_t *cur = heap_start_ptr;
    while (cur && cur != hdr) {
        prev = cur;
        cur = cur->next;
    }

    coalesce_from(hdr);
    if (prev && prev->free)
        coalesce_from(prev);

    //heap_trim(); // give back completely free top space
}

// ── Statistics ─────────────────────────────────────────────────────────
uint32_t heap_used(void) {
    uint32_t used = 0;
    block_header_t *cur = heap_start_ptr;
    while (cur) {
        if (cur->magic != BLOCK_MAGIC_FREE && cur->magic != BLOCK_MAGIC_USED)
            break;
        if (!cur->free)
            used += HEADER_SIZE + cur->size;
        cur = cur->next;
    }
    return used;
}

uint32_t heap_remaining(void) {
    uint32_t free_bytes = 0;
    block_header_t *cur = heap_start_ptr;
    while (cur) {
        if (cur->magic != BLOCK_MAGIC_FREE && cur->magic != BLOCK_MAGIC_USED)
            break;
        if (cur->free)
            free_bytes += cur->size;
        cur = cur->next;
    }
    return free_bytes;
}

// ── Pool allocator ─────────────────────────────────────────
bool pool_init(pool_t *pool, void *slab, uint32_t capacity,
               uint32_t block_size) {
    if (!pool || !slab || capacity == 0 || block_size == 0)
        return false;

    pool->block_size = (block_size + 3) & ~3u;
    pool->capacity = capacity;
    pool->slab = (uint8_t *)slab;

    pool->free_stack = (uint32_t *)kmalloc(capacity * sizeof(uint32_t));
    if (!pool->free_stack)
        return false;

    for (uint32_t i = 0; i < capacity; i++)
        pool->free_stack[i] = i;
    pool->free_count = capacity;
    return true;
}

void pool_destroy(pool_t *pool) {
    if (pool && pool->free_stack) {
        kfree(pool->free_stack);
        pool->free_stack = NULL;
    }
}

void *pool_alloc(pool_t *pool) {
    if (!pool || pool->free_count == 0)
        return NULL;
    uint32_t idx = pool->free_stack[--pool->free_count];
    return pool->slab + idx * pool->block_size;
}

void pool_free(pool_t *pool, void *ptr) {
    if (!pool || !ptr)
        return;
    uint32_t idx = (uint32_t)((uint8_t *)ptr - pool->slab) / pool->block_size;
    if (idx < pool->capacity)
        pool->free_stack[pool->free_count++] = idx;
}
