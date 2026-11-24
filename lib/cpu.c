// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "lib/cpu.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

static char s_model[49];
static uint32_t s_usage = 0;
static uint32_t s_idle_baseline = 0;
static uint32_t s_idle_count = 0;
static uint32_t s_last_ms = 0;
static bool s_measuring = false;

// CPU / FPU detection (safe on a 386: no CPUID until we know it exists)

static bool s_has_cpuid, s_has_fpu;
static int s_family; /* 3 = 386, 4 = 486, 5 = Pentium, 6+ = later */

/* Can EFLAGS bit `bit` be toggled? (AC = 18 exists from the 486, ID = 21
 * means CPUID is available). */
static bool eflags_toggles(uint32_t bit) {
    uint32_t a, b;
    __asm__ volatile("pushfl\n\t"
                     "pushfl\n\t"
                     "popl %0\n\t"
                     "movl %0, %1\n\t"
                     "xorl %2, %0\n\t"
                     "pushl %0\n\t"
                     "popfl\n\t"
                     "pushfl\n\t"
                     "popl %0\n\t"
                     "popfl"
                     : "=&r"(a), "=&r"(b)
                     : "ri"(1u << bit)
                     : "cc");
    return ((a ^ b) & (1u << bit)) != 0;
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c,
                  uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

/* x87 present? FNINIT then FNSTSW must give 0 (no-wait forms don't hang
 * on a 386 without a coprocessor). */
static bool detect_fpu(void) {
    uint16_t sw = 0x5A5A;
    __asm__ volatile("fninit\n\tfnstsw %0" : "+m"(sw));
    return sw == 0;
}

static void detect_cpu(void) {
    char *p = s_model;
    s_has_cpuid = eflags_toggles(21);
    if (!s_has_cpuid) {
        s_family = eflags_toggles(18) ? 4 : 3;
        strcpy(s_model, s_family == 3 ? "386-class CPU" : "486-class CPU");
        return;
    }
    uint32_t a, b, c, d, max;
    cpuid(0, &max, &b, &c, &d);
    char vendor[13];
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    s_family = 4;
    if (max >= 1) {
        cpuid(1, &a, &b, &c, &d);
        s_family = (a >> 8) & 0xF;
        if (s_family == 0xF)
            s_family += (a >> 20) & 0xFF;
    }
    uint32_t ext;
    cpuid(0x80000000, &ext, &b, &c, &d);
    if (ext >= 0x80000004 && ext < 0x8000FFFF) { /* brand string */
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid(leaf, &a, &b, &c, &d);
            memcpy(p, &a, 4);
            memcpy(p + 4, &b, 4);
            memcpy(p + 8, &c, 4);
            memcpy(p + 12, &d, 4);
            p += 16;
        }
        s_model[48] = '\0';
        char *start = s_model;
        while (*start == ' ')
            start++;
        if (start != s_model)
            memmove(s_model, start, strlen(start) + 1);
    } else { /* 486 with CPUID, Pentium, Pentium MMX/Pro/II... */
        snprintf(s_model, sizeof(s_model), "%s family %d", vendor, s_family);
    }
}

void cpu_init(void) {
    detect_cpu();
    s_has_fpu = detect_fpu();
    /* CR0: without a coprocessor set EM so x87 instructions fault (#NM)
     * instead of hanging; with one, clear EM and set MP. */
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    if (s_has_fpu)
        cr0 = (cr0 & ~(1u << 2)) | (1u << 1);
    else
        cr0 = (cr0 | (1u << 2)) & ~(1u << 1);
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
}

bool cpu_has_fpu(void) { return s_has_fpu; }

static bool s_forced;
bool cpu_fpu_emulated(void) { return !s_has_fpu || s_forced; }

void cpu_emulate_fpu(void) {
    if (!s_has_fpu || s_forced)
        return;
    s_forced = true;
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 | (1u << 2)) & ~(1u << 1); /* EM on, MP off: x87 traps */
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
}
int cpu_family(void) { return s_family; }

void cpu_model_init(void) {
    if (!s_model[0])
        detect_cpu();
}

void cpu_metrics_init(void) {
    cpu_model_init();

    uint32_t count = 0;
    uint32_t start = time_millis();
    while (time_millis() - start < 50)
        count++;
    s_idle_baseline = count * 20;
    if (s_idle_baseline == 0)
        s_idle_baseline = 1;

    s_last_ms = time_millis();
    s_idle_count = 0;
    s_measuring = true;
}

void cpu_metrics_update(void) {
    s_idle_count++;
    uint32_t now = time_millis();
    if (now - s_last_ms >= 1000) {
        uint32_t idle_pct = (s_idle_count * 100) / s_idle_baseline;
        if (idle_pct > 100)
            idle_pct = 100;
        s_usage = 100 - idle_pct;
        s_idle_count = 0;
        s_last_ms = now;
    }
}

uint32_t cpu_usage_percent(void) { return s_usage; }
const char *cpu_model_str(void) { return s_model; }
