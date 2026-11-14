#ifndef VMM_H
#define VMM_H

#include "lib/core.h"

/* NOTE: pmm_init()/vmm_init() are never called from kmain, so
 * vmm_create_context() always fails and shell `run` falls back to
 * "[no isolation]". Kept as the start of per-task isolation; wire up the
 * init calls (after alloc_set_range) to activate it. */

// Constants
#define PAGE_SIZE       4096u
#define PAGE_PRESENT    (1u << 0)
#define PAGE_WRITABLE   (1u << 1)
#define PAGE_USER       (1u << 2)
#define PAGE_PS         (1u << 7)

#define KERNEL_IDENTITY_MB   64u
#define KERNEL_IDENTITY_4MB  (KERNEL_IDENTITY_MB / 4)
#define PROC_PRIVATE_VADDR_BASE  0x01000000u

// Types
typedef uint32_t page_dir_t[1024];

typedef struct {
    page_dir_t *pd;          // physical address of the PD
    uint32_t    pd_phys;     // same value; explicit for clarity
    uint32_t    private_phys;// physical base of private heap frames
    uint32_t    private_pages;// how many pages were allocated
} vmm_ctx_t;

// API
bool vmm_init(void);
vmm_ctx_t *vmm_create_context(uint32_t private_pages);
void vmm_destroy_context(vmm_ctx_t *ctx);
void vmm_load_context(const vmm_ctx_t *ctx);
void vmm_load_kernel(void);
bool vmm_map_page(vmm_ctx_t *ctx, uint32_t virt, uint32_t phys, uint32_t flags);
extern uint32_t g_kernel_pd_phys;

#endif
