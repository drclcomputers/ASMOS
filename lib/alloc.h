#ifndef ALLOC_H
#define ALLOC_H

#include "lib/types.h"
#include "config/config.h"


void alloc_init(void);
void alloc_set_end(uint32_t end);
void *kmalloc(uint32_t size);
void kfree(void *ptr);
uint32_t heap_used(void);
uint32_t heap_remaining(void);

#endif
