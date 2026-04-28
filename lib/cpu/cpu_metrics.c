#include "lib/cpu/cpu_metrics.h"
#include "lib/string.h"
#include "lib/memory.h"
#include "lib/time.h"

static char s_model[49];
static uint32_t s_usage = 0;
static uint32_t s_idle_baseline = 0;
static uint32_t s_idle_count = 0;
static uint32_t s_last_ms = 0;
static bool s_measuring = false;

static void read_cpuid_model(void) {
    uint32_t eax, ebx, ecx, edx;
    char *p = s_model;

    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        __asm__ volatile(
            "cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(leaf)
        );
        memcpy(p,      &eax, 4);
        memcpy(p + 4,  &ebx, 4);
        memcpy(p + 8,  &ecx, 4);
        memcpy(p + 12, &edx, 4);
        p += 16;
    }
    s_model[48] = '\0';

    char *start = s_model;
    while (*start == ' ') start++;
    if (start != s_model)
        memmove(s_model, start, strlen(start) + 1);
}

static uint32_t check_extended_cpuid(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000));
    return eax;
}

void cpu_model_init(void) {
    if (check_extended_cpuid() >= 0x80000004)
        read_cpuid_model();
    else
        strncpy(s_model, "Unknown CPU", 48);
}

void cpu_metrics_init(void) {
    cpu_model_init();

    uint32_t count = 0;
    uint32_t start = time_millis();
    while (time_millis() - start < 50)
        count++;
    s_idle_baseline = count * 20;
    if (s_idle_baseline == 0) s_idle_baseline = 1;

    s_last_ms = time_millis();
    s_idle_count = 0;
    s_measuring = true;
}

void cpu_metrics_update(void) {
    s_idle_count++;
    uint32_t now = time_millis();
    if (now - s_last_ms >= 1000) {
        uint32_t idle_pct = (s_idle_count * 100) / s_idle_baseline;
        if (idle_pct > 100) idle_pct = 100;
        s_usage = 100 - idle_pct;
        s_idle_count = 0;
        s_last_ms = now;
    }
}

uint32_t cpu_usage_percent(void) { return s_usage; }
const char *cpu_model_str(void) { return s_model; }
