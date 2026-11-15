#include "lib/vmm.h"
#include "lib/cpu.h"
#include "lib/memory.h"

uint32_t g_kernel_pd_phys = 0;
static page_dir_t *s_kernel_pd;
static uint32_t s_map_top;

/* linker.ld: kernel .text + .rodata, page aligned */
extern uint8_t _ro_start, _ro_end;

static inline void cr3_load(uint32_t pd) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd) : "memory");
}

bool vmm_enabled(void) { return g_kernel_pd_phys != 0; }

uint32_t vmm_current(void) {
    if (!vmm_enabled())
        return 0;
    uint32_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

void vmm_switch(uint32_t cr3) {
    if (cr3 && vmm_enabled() && cr3 != vmm_current())
        cr3_load(cr3);
}

void vmm_load_context(const vmm_ctx_t *ctx) {
    if (ctx && ctx->pd_phys)
        vmm_switch(ctx->pd_phys);
}

void vmm_load_kernel(void) { vmm_switch(g_kernel_pd_phys); }

/* ── kernel address space ─────────────────────────────────────────────── */

bool vmm_init(uint32_t map_top) {
    if (vmm_enabled() || !pmm_total())
        return false;
    if (map_top > VMM_MAX_MAP_MB * 0x100000u)
        map_top = VMM_MAX_MAP_MB * 0x100000u;
    map_top = (map_top + 0x3FFFFFu) & ~0x3FFFFFu; /* whole page tables */
    if (map_top < 0x400000u)
        map_top = 0x400000u;
    uint32_t npt = map_top >> 22;

    /* PD + PTs live in the kernel heap (permanent, so the alignment slack
     * kmalloc_aligned leaves behind does not matter) */
    uint8_t *mem = (uint8_t *)kmalloc_aligned((npt + 1) * PAGE_SIZE, PAGE_SIZE);
    if (!mem)
        return false;
    memset(mem, 0, (npt + 1) * PAGE_SIZE);
    page_dir_t *pd = (page_dir_t *)mem;
    for (uint32_t t = 0; t < npt; t++) {
        uint32_t *pt = (uint32_t *)(mem + (t + 1) * PAGE_SIZE);
        for (uint32_t i = 0; i < 1024; i++)
            pt[i] = ((t << 22) | (i << 12)) | PAGE_WRITABLE | PAGE_PRESENT;
        (*pd)[t] = (uint32_t)(uintptr_t)pt | PAGE_WRITABLE | PAGE_PRESENT;
    }
    s_kernel_pd = pd;
    s_map_top = map_top;
    g_kernel_pd_phys = (uint32_t)(uintptr_t)pd;

    /* PG, plus WP on 486+ so read-only pages also bind ring 0 (the 386
     * has no WP bit; leave its reserved bits alone) */
    uint32_t bits = 0x80000000u | (cpu_family() >= 4 ? 0x10000u : 0);
    cr3_load(g_kernel_pd_phys);
    __asm__ volatile("mov %%cr0, %%eax\n\t"
                     "or  %0, %%eax\n\t"
                     "mov %%eax, %%cr0\n\t"
                     "jmp 1f\n" /* 386: flush the prefetch queue */
                     "1:\n\t" ::"r"(bits)
                     : "eax", "memory");
    return true;
}

/* ── contexts ─────────────────────────────────────────────────────────── */

static uint32_t *pt_of(const vmm_ctx_t *ctx, uint32_t virt) {
    uint32_t pde = (*ctx->pd)[virt >> 22];
    if (!(pde & PAGE_PRESENT))
        return NULL;
    return (uint32_t *)(uintptr_t)(pde & ~0xFFFu);
}

static bool own_pt(const vmm_ctx_t *ctx, uint32_t pt) {
    for (int i = 0; i < ctx->npt; i++)
        if (ctx->pt[i] == pt)
            return true;
    return false;
}

/* Give ctx its own copy of the page table for PDE `pdi`. */
static uint32_t *privatize(vmm_ctx_t *ctx, uint32_t pdi) {
    uint32_t pde = (*ctx->pd)[pdi];
    uint32_t cur = pde & ~0xFFFu;
    if ((pde & PAGE_PRESENT) && own_pt(ctx, cur))
        return (uint32_t *)(uintptr_t)cur;
    if (ctx->npt >= VMM_MAX_PRIV_PT)
        return NULL;
    uint32_t pt = pmm_alloc_frame(); /* zeroed */
    if (!pt)
        return NULL;
    if (pde & PAGE_PRESENT)
        memcpy((void *)(uintptr_t)pt, (void *)(uintptr_t)cur, PAGE_SIZE);
    ctx->pt[ctx->npt++] = pt;
    (*ctx->pd)[pdi] = pt | PAGE_WRITABLE | PAGE_PRESENT;
    return (uint32_t *)(uintptr_t)pt;
}

/* Page tables and pool frames are only mapped in the kernel space: work
 * on them there, whatever context the caller is in. */
#define KERNEL_SPACE_BEGIN                                                     \
    uint32_t _cr3 = vmm_current();                                             \
    vmm_load_kernel()
#define KERNEL_SPACE_END vmm_switch(_cr3)

static vmm_ctx_t *create(uint32_t private_pages);

vmm_ctx_t *vmm_create_context(uint32_t private_pages) {
    if (!vmm_enabled())
        return NULL;
    KERNEL_SPACE_BEGIN;
    vmm_ctx_t *ctx = create(private_pages);
    KERNEL_SPACE_END;
    return ctx;
}

static vmm_ctx_t *create(uint32_t private_pages) {
    vmm_ctx_t *ctx = (vmm_ctx_t *)kzalloc(sizeof(vmm_ctx_t));
    if (!ctx)
        return NULL;
    ctx->pd_phys = pmm_alloc_frame();
    if (!ctx->pd_phys) {
        kfree(ctx);
        return NULL;
    }
    ctx->pd = (page_dir_t *)(uintptr_t)ctx->pd_phys;
    memcpy(ctx->pd, s_kernel_pd, sizeof(page_dir_t));

    /* tables that differ from the kernel's: the first 4 MB (page 0,
     * kernel code) and the ones covering the frame pool */
    uint32_t ro0 = (uint32_t)(uintptr_t)&_ro_start;
    uint32_t ro1 = (uint32_t)(uintptr_t)&_ro_end;
    uint32_t p0 = pmm_base() >> 22, p1 = (pmm_end() - 1) >> 22;
    uint32_t list[VMM_MAX_PRIV_PT];
    int n = 0;
    list[n++] = 0;
    for (uint32_t pdi = p0; pdi <= p1 && n < VMM_MAX_PRIV_PT - 1; pdi++)
        if (pdi != 0)
            list[n++] = pdi;
    for (int k = 0; k < n; k++) {
        uint32_t pdi = list[k];
        uint32_t *pt = privatize(ctx, pdi);
        if (!pt) {
            vmm_destroy_context(ctx);
            return NULL;
        }
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t va = (pdi << 22) | (i << 12);
            if (va < PAGE_SIZE || pmm_owns(va))
                pt[i] = 0; /* NULL page, other apps, page tables */
            else if (va >= ro0 && va < ro1)
                pt[i] &= ~PAGE_WRITABLE;
        }
    }

    if (private_pages > 0) {
        uint32_t phys = pmm_alloc_frames(private_pages);
        if (!phys) {
            vmm_destroy_context(ctx);
            return NULL;
        }
        ctx->private_phys = phys;
        ctx->private_pages = private_pages;
        for (uint32_t p = 0; p < private_pages; p++)
            if (!vmm_map_page(ctx, PROC_PRIVATE_VADDR_BASE + p * PAGE_SIZE,
                              phys + p * PAGE_SIZE,
                              PAGE_PRESENT | PAGE_WRITABLE)) {
                vmm_destroy_context(ctx);
                return NULL;
            }
    }
    return ctx;
}

bool vmm_grant(vmm_ctx_t *ctx, uint32_t addr, uint32_t pages, bool writable) {
    if (!ctx)
        return false;
    KERNEL_SPACE_BEGIN;
    bool ok = true;
    for (uint32_t p = 0; p < pages && ok; p++) {
        uint32_t va = (addr & ~0xFFFu) + p * PAGE_SIZE;
        if (!pmm_owns(va))
            continue; /* everything else is already mapped */
        uint32_t *pt = pt_of(ctx, va);
        if (!pt || !own_pt(ctx, (uint32_t)(uintptr_t)pt))
            ok = false;
        else
            pt[(va >> 12) & 0x3FF] =
                va | PAGE_PRESENT | (writable ? PAGE_WRITABLE : 0);
    }
    KERNEL_SPACE_END; /* a CR3 load also flushes the TLB (no INVLPG on 386) */
    return ok;
}

bool vmm_map_page(vmm_ctx_t *ctx, uint32_t virt, uint32_t phys,
                  uint32_t flags) {
    if (!ctx || !ctx->pd)
        return false;
    uint32_t *pt = privatize(ctx, virt >> 22);
    if (!pt)
        return false;
    pt[(virt >> 12) & 0x3FF] = (phys & ~0xFFFu) | (flags & 0xFFFu);
    return true;
}

void vmm_destroy_context(vmm_ctx_t *ctx) {
    if (!ctx)
        return;
    uint32_t back = vmm_current();
    if (back == ctx->pd_phys)
        back = g_kernel_pd_phys; /* it is going away */
    vmm_load_kernel();
    if (ctx->private_phys)
        pmm_free_frames(ctx->private_phys, ctx->private_pages);
    for (int i = 0; i < ctx->npt; i++)
        pmm_free_frame(ctx->pt[i]);
    if (ctx->pd_phys)
        pmm_free_frame(ctx->pd_phys);
    kfree(ctx);
    vmm_switch(back);
}
