#include "lib/alloc.h"
#include "config/config.h"
#include "lib/mem.h"

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

/* The kernel heap. Dynamic apps get heaps of their own (os/appload.c), so
 * an app overrunning a block breaks only its own heap. */
heap_t g_kheap;

#define FIRST(h) ((block_header_t *)(h)->first)

// ── Heaps ─────────────────────────────────────────────────────────────
bool heap_init(heap_t *h, uint32_t start, uint32_t end) {
    if (!h)
        return false;
    start = (start + 3) & ~3u;
    /* need room for one header plus one usable word, or size wraps */
    if (end <= start || (end - start) <= HEADER_SIZE + 4) {
        h->first = NULL;
        h->start = h->end = 0;
        return false;
    }
    h->start = start;
    h->end = end;
    block_header_t *b = (block_header_t *)(uintptr_t)start;
    b->magic = BLOCK_MAGIC_FREE;
    b->size = end - start - HEADER_SIZE;
    b->free = true;
    b->next = NULL;
    h->first = b;
    return true;
}

bool heap_owns(const heap_t *h, const void *p) {
    uint32_t a = (uint32_t)(uintptr_t)p;
    return h && h->first && a >= h->start + HEADER_SIZE && a < h->end;
}

/* A header is believable if it is inside the heap with a known magic. */
static bool sane(const heap_t *h, const block_header_t *b) {
    uint32_t a = (uint32_t)(uintptr_t)b;
    return a >= h->start && a + HEADER_SIZE <= h->end &&
           (b->magic == BLOCK_MAGIC_FREE || b->magic == BLOCK_MAGIC_USED) &&
           b->size <= h->end - a - HEADER_SIZE;
}

static void coalesce_from(const heap_t *h, block_header_t *cur) {
    // Merge consecutive free blocks starting at cur; a used or corrupt
    // block stops the walk.
    while (cur) {
        if (cur->magic != BLOCK_MAGIC_FREE)
            return;
        block_header_t *nx = cur->next;
        if (!nx)
            break;
        if (!sane(h, nx))
            return;
        if (!nx->free)
            break;
        cur->size += HEADER_SIZE + nx->size;
        cur->next = nx->next;
    }
}

void *heap_alloc(heap_t *h, uint32_t size) {
    if (!h || !h->first || size == 0 || size > h->end - h->start)
        return NULL;
    size = (size + 3) & ~3u;

    block_header_t *cur = FIRST(h);
    while (cur) {
        if (!sane(h, cur))
            return NULL; /* corrupted: refuse rather than spread it */
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

/* Pointers this heap did not hand out are ignored. */
void heap_free(heap_t *h, void *ptr) {
    if (!ptr || !heap_owns(h, ptr))
        return;
    block_header_t *hdr = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (hdr->magic != BLOCK_MAGIC_USED)
        return;

    // Find the predecessor (also proves hdr is a real block of this heap).
    block_header_t *prev = NULL;
    block_header_t *cur = FIRST(h);
    while (cur && cur != hdr) {
        if (!sane(h, cur))
            return;
        prev = cur;
        cur = cur->next;
    }
    if (cur != hdr)
        return;

    hdr->free = true;
    hdr->magic = BLOCK_MAGIC_FREE;
    coalesce_from(h, hdr); // forward merge
    if (prev && prev->magic == BLOCK_MAGIC_FREE && prev->free)
        coalesce_from(h, prev); // backward merge
}

static uint32_t heap_sum(const heap_t *h, bool want_free) {
    uint32_t n = 0;
    if (!h)
        return 0;
    for (block_header_t *cur = FIRST(h); cur; cur = cur->next) {
        if (!sane(h, cur))
            break;
        if (cur->free == want_free)
            n += want_free ? cur->size : HEADER_SIZE + cur->size;
    }
    return n;
}

uint32_t heap_used_in(const heap_t *h) { return heap_sum(h, false); }
uint32_t heap_free_in(const heap_t *h) { return heap_sum(h, true); }

/* Walks the whole heap: false if any header is broken. */
bool heap_check(const heap_t *h) {
    if (!h || !h->first)
        return false;
    int guard = 0;
    for (block_header_t *cur = FIRST(h); cur; cur = cur->next) {
        if (!sane(h, cur) || ++guard > 1000000)
            return false;
        if (cur->next &&
            (uint8_t *)cur->next != (uint8_t *)cur + HEADER_SIZE + cur->size)
            return false;
    }
    return true;
}

// ── Kernel heap ───────────────────────────────────────────────────────
void alloc_set_range(uint32_t start, uint32_t end) {
    if (start < (uint32_t)&_heap_start)
        start = (uint32_t)&_heap_start;
    heap_init(&g_kheap, start, end);
}

void alloc_set_end(uint32_t end) {
    alloc_set_range((uint32_t)&_heap_start, end);
}

void alloc_init(void) {
    alloc_set_range((uint32_t)&_heap_start, (uint32_t)&_heap_start + 0x100000);
}

void *kmalloc(uint32_t size) { return heap_alloc(&g_kheap, size); }

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
    if (!heap_owns(&g_kheap, ptr))
        return NULL;
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
    if (align == 0 || (align & (align - 1)) != 0)
        return NULL;
    if (align <= 4)
        return kmalloc(size);
    // The block is over-allocated and the aligned address inside it is
    // returned; it cannot be kfree'd (kfree ignores it). For permanent
    // buffers only.
    uint8_t *raw = (uint8_t *)kmalloc(size + (align - 1));
    if (!raw)
        return NULL;
    uintptr_t addr = (uintptr_t)raw;
    return (void *)((addr + align - 1) & ~((uintptr_t)align - 1));
}

/* Ignores pointers outside the kernel heap (app heaps, static data). */
void kfree(void *ptr) { heap_free(&g_kheap, ptr); }

uint32_t heap_used(void) { return heap_used_in(&g_kheap); }
uint32_t heap_remaining(void) { return heap_free_in(&g_kheap); }

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
