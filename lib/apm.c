#include "lib/apm.h"
#include "lib/time.h"
#include "lib/vmm.h"

extern uint8_t kernel_gdt[]; /* boot/loader.asm */

#define APM_CS 0x18 /* then 16-bit code at 0x20, data at 0x28 */

static struct __attribute__((packed)) {
    uint32_t offset;
    uint16_t selector;
} s_entry;
static uint16_t s_version;

static void set_desc(int sel, uint32_t base, uint32_t limit, uint8_t access,
                     uint8_t flags) {
    uint8_t *d = kernel_gdt + sel;
    d[0] = (uint8_t)limit;
    d[1] = (uint8_t)(limit >> 8);
    d[2] = (uint8_t)base;
    d[3] = (uint8_t)(base >> 8);
    d[4] = (uint8_t)(base >> 16);
    d[5] = access;
    d[6] = (uint8_t)((flags << 4) | ((limit >> 16) & 0x0F));
    d[7] = (uint8_t)(base >> 24);
}

/* One APM function through the 32-bit interface. False: carry set (the
 * BIOS said no). The BIOS may use any register, and loads its own data
 * segment; FS/GS go in empty, as the spec asks. */
static bool apm_call(uint16_t ax, uint16_t bx, uint16_t cx) {
    uint32_t a = ax, b = bx, c = cx, d = 0, S = 0, D = 0;
    uint32_t fl;
    __asm__ volatile("pushfl\n\t"
                     "popl %0\n\t"
                     "cli"
                     : "=r"(fl)::"memory");
    __asm__ volatile("pushl %%ebp\n\t"
                     "pushl %%ds\n\t"
                     "pushl %%es\n\t"
                     "pushl %%fs\n\t"
                     "pushl %%gs\n\t"
                     "pushl $0\n\t"
                     "popl %%fs\n\t"
                     "pushl $0\n\t"
                     "popl %%gs\n\t"
                     "lcall *%6\n\t"
                     "pushfl\n\t"
                     "popl %%esi\n\t"
                     "popl %%gs\n\t"
                     "popl %%fs\n\t"
                     "popl %%es\n\t"
                     "popl %%ds\n\t"
                     "popl %%ebp"
                     : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(S), "+D"(D)
                     : "m"(s_entry)
                     : "memory", "cc");
    __asm__ volatile("pushl %0\n\t"
                     "popfl" ::"r"(fl)
                     : "memory", "cc");
    return !(S & 1); /* CF */
}

/* what stage 2 left at 0x0610 (bootdefs.inc); the pointer goes through
 * an asm so the compiler doesn't treat low addresses as out of bounds */
static uint32_t boot_word(uint32_t addr, int bytes) {
    uintptr_t p = addr;
    __asm__("" : "+r"(p));
    return bytes == 4 ? *(volatile uint32_t *)p : *(volatile uint16_t *)p;
}

void apm_init(void) {
    uint16_t ver = (uint16_t)boot_word(0x0610, 2);
    if (!ver)
        return;
    uint32_t cs32 = boot_word(0x0614, 2);
    uint32_t cs16 = boot_word(0x0616, 2);
    uint32_t ds = boot_word(0x0618, 2);
    uint32_t entry = boot_word(0x061A, 4);
    /* 64 KB limits whatever the BIOS says: some report them too short */
    set_desc(APM_CS, cs32 << 4, 0xFFFF, 0x9A, 0x4);     /* 32-bit code */
    set_desc(APM_CS + 8, cs16 << 4, 0xFFFF, 0x9A, 0x0); /* 16-bit code */
    set_desc(APM_CS + 16, ds << 4, 0xFFFF, 0x92, 0x4);  /* data */
    s_entry.offset = entry;
    s_entry.selector = APM_CS;
    s_version = ver;
    /* say which APM we speak (1.0 BIOSes don't know this call) */
    if (ver >= 0x0101)
        apm_call(0x530E, 0x0000, ver >= 0x0102 ? 0x0102 : 0x0101);
}

bool apm_present(void) { return s_version != 0; }
uint16_t apm_version(void) { return s_version; }

void apm_power_off(void) {
    if (!s_version)
        return;
    vmm_load_kernel(); /* the BIOS is below 1 MB: mapped 1:1 there */
    apm_call(0x5308, 0x0001, 0x0001); /* power management on */
    if (s_version >= 0x0101)
        apm_call(0x530D, 0x0001, 0x0001);  /* engaged */
    if (!apm_call(0x5307, 0x0001, 0x0003)) /* all devices: off */
        return;
    sleep_ms(1500); /* the supply may take a moment */
}
