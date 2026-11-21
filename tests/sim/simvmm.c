/* Sim stand-in for lib/vmm.c: an app context = the frame pool made
 * inaccessible (mprotect) except the ranges granted to it. Touching
 * another app's memory while its context is loaded => SIGSEGV. */
#include "sys.h"
#include "lib/memory.h"

#define SYS_mprotect 125
uint32_t g_kernel_pd_phys = 0;
static uint32_t s_cur;

typedef struct { uint32_t a, n; } grant_t;
typedef struct { vmm_ctx_t v; grant_t g[4]; int ng; } sctx_t;

void simvmm_on(void) { g_kernel_pd_phys = 1; s_cur = 1; }
bool vmm_enabled(void) { return g_kernel_pd_phys != 0; }
uint32_t vmm_current(void) { return vmm_enabled() ? s_cur : 0; }
bool vmm_init(uint32_t t) { (void)t; return false; }

static void prot(uint32_t a, uint32_t len, int p) { sys3(SYS_mprotect, (int)a, (int)len, p); }

volatile uint32_t g_flush_mark; uint32_t g_cr3_loads;
void vmm_switch(uint32_t cr3) {
    if (!cr3 || !vmm_enabled() || cr3 == s_cur) return;
    (void)g_flush_mark; g_cr3_loads++; /* (TLB measurement: a CR3 load) */
    s_cur = cr3;
    uint32_t b = pmm_base(), e = pmm_end();
    if (cr3 == 1) { prot(b, e - b, 7); return; }
    sctx_t *c = (sctx_t *)cr3;
    prot(b, e - b, 0);
    for (int i = 0; i < c->ng; i++) prot(c->g[i].a, c->g[i].n * 4096, 7);
}
void vmm_load_context(const vmm_ctx_t *ctx) { if (ctx) vmm_switch((uint32_t)ctx); }
void vmm_load_kernel(void) { vmm_switch(1); }

vmm_ctx_t *vmm_create_context(uint32_t pp) {
    if (!vmm_enabled() || pp) return NULL;
    sctx_t *c = kzalloc(sizeof(sctx_t));
    if (c) c->v.pd_phys = (uint32_t)c;
    return (vmm_ctx_t *)c;
}
bool vmm_grant(vmm_ctx_t *ctx, uint32_t a, uint32_t n, bool w) {
    (void)w; sctx_t *c = (sctx_t *)ctx;
    if (!c || c->ng >= 4) return false;
    c->g[c->ng].a = a & ~0xFFFu; c->g[c->ng].n = n; c->ng++;
    return true;
}
void vmm_destroy_context(vmm_ctx_t *ctx) {
    if (!ctx) return;
    if (s_cur == (uint32_t)ctx) vmm_load_kernel();
    kfree(ctx);
}
bool vmm_map_page(vmm_ctx_t *c, uint32_t v, uint32_t p, uint32_t f) { (void)c; (void)v; (void)p; (void)f; return false; }

// a fake timer interrupt: SIGALRM aborts the running guarded call
#include "os/appguard.h"
#define SYS_rt_sigaction 174
#define SYS_setitimer 104
struct ksa { void (*h)(int); unsigned long flags; void (*restorer)(void); unsigned long mask[2]; };
extern void sim_sigret(void);
__asm__(".globl sim_sigret\nsim_sigret:\n mov $173, %eax\n int $0x80\n");
uint32_t g_sim_abort_vec;
static void on_alarm(int sig) {
    (void)sig;
    if (g_sim_abort_vec && appguard_active()) {
        uint32_t v = g_sim_abort_vec;
        g_sim_abort_vec = 0;
        appguard_abort(v); /* longjmps out of the handler */
    }
}
void sim_alarm_ms(int ms) {
    static struct ksa sa;
    sa.h = on_alarm;
    sa.flags = 0x04000000 | 0x40000000; /* SA_RESTORER | SA_NODEFER */
    sa.restorer = sim_sigret;
    sys6(SYS_rt_sigaction, 14, (int)&sa, 0, 8, 0, 0);
    static int it[4];
    it[0] = 0; it[1] = 0; it[2] = ms / 1000; it[3] = (ms % 1000) * 1000;
    sys3(SYS_setitimer, 0, (int)it, 0);
}
