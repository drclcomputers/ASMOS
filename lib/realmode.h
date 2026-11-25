// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef REALMODE_H
#define REALMODE_H

/* BIOS calls from the kernel (lib/realmode.asm): down to real mode, INT n,
 * back. Every IRQ is masked meanwhile, so keep them short: the video BIOS's
 * bank switch (VESA 4F05h) is what it is for. Uses 0x1000-0x1FFF of low
 * memory (boot/rmdefs.inc). */

#include "lib/core.h"

typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi;
    uint16_t ds, es, flags, pad; /* flags: out only (CF = bit 0) */
} rm_regs_t;

void realmode_install(void); /* kmain, early: the stub to low memory */
void realmode_int(uint8_t n, rm_regs_t *r);

#endif
