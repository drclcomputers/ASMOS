#include "lib/alloc.h"
#include "lib/mem.h"

typedef struct block_header {
    uint32_t            size;
    bool                free;
    struct block_header *next;
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)

static block_header_t *heap_start_ptr = NULL;
static uint32_t heap_end = HEAP_END;

void alloc_init(void) {
    heap_start_ptr = (block_header_t *)HEAP_START;
    heap_start_ptr->size = heap_end - HEAP_START - HEADER_SIZE;
    heap_start_ptr->free = true;
    heap_start_ptr->next = NULL;
}

void alloc_set_end(uint32_t end) {
    heap_end = end;
    alloc_init();
}

static void coalesce(void) {
    block_header_t *cur = heap_start_ptr;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += HEADER_SIZE + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

void *kmalloc(uint32_t size) {
    if (size == 0) return NULL;

    // align to 4 bytes
    size = (size + 3) & ~3;

    block_header_t *cur = heap_start_ptr;

    while (cur) {
        if (cur->free && cur->size >= size) {
            if (cur->size >= size + HEADER_SIZE + 4) {
                block_header_t *new_block = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + size);
                new_block->size = cur->size - size - HEADER_SIZE;
                new_block->free = true;
                new_block->next = cur->next;

                cur->size = size;
                cur->next = new_block;
            }

            cur->free = false;
            return (void *)((uint8_t *)cur + HEADER_SIZE);
        }
        cur = cur->next;
    }

    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) return;

    block_header_t *hdr = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    hdr->free = true;

    coalesce();
}

uint32_t heap_used(void) {
    uint32_t used = 0;
    block_header_t *cur = heap_start_ptr;
    while (cur) {
        if (!cur->free) used += HEADER_SIZE + cur->size;
        cur = cur->next;
    }
    return used;
}

uint32_t heap_remaining(void) {
    uint32_t free_bytes = 0;
    block_header_t *cur = heap_start_ptr;
    while (cur) {
        if (cur->free) free_bytes += cur->size;
        cur = cur->next;
    }
    return free_bytes;
}
