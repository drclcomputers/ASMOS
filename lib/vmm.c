#include "lib/cpu.h"
#include "lib/memory.h"

uint32_t g_kernel_pd_phys = 0;
static page_dir_t *s_kernel_pd = NULL;

// Internal helpers
static inline void cr3_load(uint32_t phys_pd) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys_pd) : "memory");
}

static void enable_paging_pse(uint32_t pd_phys) {
    __asm__ volatile("mov %%cr4, %%eax\n\t"
                     "or  $0x10, %%eax\n\t" // CR4.PSE
                     "mov %%eax, %%cr4\n\t" ::
                         : "eax", "memory");

    cr3_load(pd_phys);

    __asm__ volatile("mov %%cr0, %%eax\n\t"
                     "or  $0x80010000, %%eax\n\t" // PG | WP
                     "mov %%eax, %%cr0\n\t" ::
                         : "eax", "memory");
}

static bool build_kernel_pd(page_dir_t *pd) {
    memset(pd, 0, sizeof(page_dir_t));

    for (uint32_t i = 0; i < KERNEL_IDENTITY_4MB; i++) {
        // Each PD entry covers 4MB starting at i * 4MB.
        uint32_t phys_base = i * 0x400000u;
        // PS=1 (4MB page), Present, Writable, Supervisor (no USER bit).
        (*pd)[i] = phys_base | PAGE_PS | PAGE_WRITABLE | PAGE_PRESENT;
    }
    return true;
}

// Ensure the page-table for a given virtual address exists inside ctx->pd.
static uint32_t *ensure_pt(page_dir_t *pd, uint32_t virt) {
    uint32_t pdi = virt >> 22;
    uint32_t pde = (*pd)[pdi];

    if (pde & PAGE_PRESENT) {
        if (pde & PAGE_PS) {
            return NULL;
        }
        return (uint32_t *)(uintptr_t)(pde & ~0xFFFu);
    }

    // Allocate a fresh PT page.
    uint32_t pt_phys = pmm_alloc_frame();
    if (!pt_phys)
        return NULL;

    memset((void *)(uintptr_t)pt_phys, 0, PAGE_SIZE);

    (*pd)[pdi] = pt_phys | PAGE_WRITABLE | PAGE_PRESENT;
    return (uint32_t *)(uintptr_t)pt_phys;
}

// Public API
bool vmm_init(void) {
    /* 4 MB pages (PSE) and CR4 exist only from the Pentium on */
    if (cpu_family() < 5)
        return false;
    // Allocate the kernel PD — must be 4KB aligned.
    uint32_t pd_phys = pmm_alloc_frame();
    if (!pd_phys)
        return false;

    s_kernel_pd = (page_dir_t *)(uintptr_t)pd_phys;
    g_kernel_pd_phys = pd_phys;

    if (!build_kernel_pd(s_kernel_pd))
        return false;

    enable_paging_pse(pd_phys);
    return true;
}

vmm_ctx_t *vmm_create_context(uint32_t private_pages) {
    vmm_ctx_t *ctx = (vmm_ctx_t *)kmalloc(sizeof(vmm_ctx_t));
    if (!ctx)
        return NULL;
    memset(ctx, 0, sizeof(vmm_ctx_t));

    // Allocate the process PD.
    uint32_t pd_phys = pmm_alloc_frame();
    if (!pd_phys) {
        kfree(ctx);
        return NULL;
    }
    ctx->pd = (page_dir_t *)(uintptr_t)pd_phys;
    ctx->pd_phys = pd_phys;

    memcpy(ctx->pd, s_kernel_pd, sizeof(page_dir_t));

    for (uint32_t i = 1; i < KERNEL_IDENTITY_4MB; i++) {
        (*ctx->pd)[i] &= ~(uint32_t)PAGE_WRITABLE;
    }

    if (private_pages > 0) {
        uint32_t priv_phys = pmm_alloc_frames(private_pages);
        if (!priv_phys) {
            pmm_free_frame(pd_phys);
            kfree(ctx);
            return NULL;
        }
        ctx->private_phys = priv_phys;
        ctx->private_pages = private_pages;

        for (uint32_t p = 0; p < private_pages; p++) {
            uint32_t virt = PROC_PRIVATE_VADDR_BASE + p * PAGE_SIZE;
            uint32_t phys = priv_phys + p * PAGE_SIZE;
            if (!vmm_map_page(ctx, virt, phys,
                              PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)) {
                for (uint32_t q = 0; q < p; q++) {
                    // (PT frames are leaked here for simplicity; fix in a
                    // future pass)
                }
                pmm_free_frames(priv_phys, private_pages);
                pmm_free_frame(pd_phys);
                kfree(ctx);
                return NULL;
            }
        }
    }

    return ctx;
}

void vmm_destroy_context(vmm_ctx_t *ctx) {
    if (!ctx)
        return;

    vmm_load_kernel();

    if (ctx->private_phys && ctx->private_pages)
        pmm_free_frames(ctx->private_phys, ctx->private_pages);

    if (ctx->pd) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t pde = (*ctx->pd)[i];
            if (!(pde & PAGE_PRESENT))
                continue;
            if (pde & PAGE_PS)
                continue; // PSE — no PT to free
            uint32_t kernel_pde = (*s_kernel_pd)[i];
            if ((pde & ~0xFFFu) == (kernel_pde & ~0xFFFu))
                continue;
            pmm_free_frame(pde & ~0xFFFu);
        }
        pmm_free_frame(ctx->pd_phys);
    }

    kfree(ctx);
}

void vmm_load_context(const vmm_ctx_t *ctx) {
    if (ctx && ctx->pd_phys)
        cr3_load(ctx->pd_phys);
}

void vmm_load_kernel(void) {
    if (g_kernel_pd_phys) /* paging never enabled: nothing to restore */
        cr3_load(g_kernel_pd_phys);
}

bool vmm_map_page(vmm_ctx_t *ctx, uint32_t virt, uint32_t phys,
                  uint32_t flags) {
    if (!ctx || !ctx->pd)
        return false;

    uint32_t *pt = ensure_pt(ctx->pd, virt);
    if (!pt)
        return false;

    uint32_t pti = (virt >> 12) & 0x3FFu;
    pt[pti] = (phys & ~0xFFFu) | (flags & 0xFFFu);
    return true;
}
