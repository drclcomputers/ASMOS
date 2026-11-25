// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef OS_MEMINFO_H
#define OS_MEMINFO_H

#include "lib/core.h"

/* Where the RAM goes, in bytes (kernel.c). They add up to total:
 *   reserved  640 KB..1 MB: video memory and the BIOS, not RAM anyone can
 *             use (it was never free)
 *   kernel    its code and data, its stack, the BIOS's data below 64 KB,
 *             the 64 KB left alone at the top
 *   screen    the back buffer, and the copy of the last frame sent
 *   heap      the kernel's heap in use: windows, fonts, menus, files...
 *   apps      the .APPs: their code and heaps
 *   free      what is left: the kernel heap's and the app pool's */
typedef struct {
    uint32_t total, reserved, kernel, screen, heap, apps, free;
} meminfo_t;

void meminfo(meminfo_t *m);

/* "1234 KB" or "3.4 MB" (from 10 MB) */
const char *meminfo_kb(uint32_t bytes, char *out, int sz);

#endif
