// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef VMM_H
#define VMM_H

#include "lib/core.h"

/*
 * 4 KB paging for 386+. App contexts leave page 0 unmapped, map only their
 * own frame-pool image/heap, and protect kernel code/constants where CR0.WP
 * is supported. Kernel RAM is mapped 1:1 and apps run in ring 0, so kernel
 * data stays reachable (apps should still use only the API).
 */

#define PAGE_SIZE 4096u
#define PAGE_PRESENT (1u << 0)
#define PAGE_WRITABLE (1u << 1)
#define PAGE_USER (1u << 2)

#define VMM_MAX_MAP_MB 64u /* identity mapping covers at most this */
#define PROC_PRIVATE_VADDR_BASE 0x40000000u /* above any mapped RAM */
#define VMM_MAX_PRIV_PT 6                   /* page tables a context may own */

typedef uint32_t page_dir_t[1024];

typedef struct {
    page_dir_t *pd;
    uint32_t pd_phys;
    uint32_t pt[VMM_MAX_PRIV_PT]; /* own page tables (pool frames) */
    int npt;
    uint32_t private_phys; /* private pages at PROC_PRIVATE_VADDR_BASE */
    uint32_t private_pages;
} vmm_ctx_t;

/* Identity-map [0, map_top) and turn paging on. Needs pmm_init first. */
bool vmm_init(uint32_t map_top);
bool vmm_enabled(void);

/* Protected context (see above); private_pages > 0 also maps that many
 * fresh pool pages at PROC_PRIVATE_VADDR_BASE (shell `run`). */
vmm_ctx_t *vmm_create_context(uint32_t private_pages);
void vmm_destroy_context(vmm_ctx_t *ctx);
/* Make pool pages [addr, addr + pages*4K) reachable in ctx. */
bool vmm_grant(vmm_ctx_t *ctx, uint32_t addr, uint32_t pages, bool writable);

void vmm_load_context(const vmm_ctx_t *ctx);
void vmm_load_kernel(void);
uint32_t vmm_current(void);    /* CR3, 0 when paging is off */
void vmm_switch(uint32_t cr3); /* load a value vmm_current() returned */
bool vmm_map_page(vmm_ctx_t *ctx, uint32_t virt, uint32_t phys, uint32_t flags);
/* Device memory (a video card's frame buffer) at its own address in the
 * kernel's space, uncached. Before any context is made: they copy it. */
bool vmm_map_device(uint32_t phys, uint32_t size);

extern uint32_t g_kernel_pd_phys;

#endif
