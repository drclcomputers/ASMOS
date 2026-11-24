// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef MEM_H
#define MEM_H

#include "lib/core.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* Dword scans with `repe/repne cmpsd` (one instruction per run, not a C
 * loop): how many leading dwords of a and b are equal / all different. */
uint32_t mem_same_run(const uint32_t *a, const uint32_t *b, uint32_t n);
uint32_t mem_diff_run(const uint32_t *a, const uint32_t *b, uint32_t n);

#endif
