#ifndef CPU_METRICS_H
#define CPU_METRICS_H

#include "lib/core.h"

/* Detect CPU (386/486 without CPUID, CPUID otherwise) and the x87 FPU.
 * Call once at boot, before anything uses floating point. */
void cpu_init(void);
bool cpu_has_fpu(void);    /* false: x87 instructions trap (CR0.EM set) */
int cpu_family(void);      /* 3 = 386, 4 = 486, 5 = Pentium, 6+ later */
void cpu_model_init(void); /* no-op after cpu_init */
/* Not called anywhere yet (Monitor only shows the model). Kept for a
 * future CPU-usage readout. */
void cpu_metrics_init(void);
void cpu_metrics_update(void);
uint32_t cpu_usage_percent(void);
const char *cpu_model_str(void);

#endif
