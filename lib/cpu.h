// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef CPU_METRICS_H
#define CPU_METRICS_H

#include "lib/core.h"

/* Detect CPU (386/486 without CPUID, CPUID otherwise) and the x87 FPU.
 * Call once at boot, before anything uses floating point. */
void cpu_init(void);
bool cpu_has_fpu(void); /* false: x87 instructions trap (CR0.EM set) */
/* x87 instructions go to the emulator (no FPU, or cpu_emulate_fpu()) */
bool cpu_fpu_emulated(void);
/* use the emulator even with an FPU (Settings, for testing) */
void cpu_emulate_fpu(void);
int cpu_family(void);      /* 3 = 386, 4 = 486, 5 = Pentium, 6+ later */
void cpu_model_init(void); /* no-op after cpu_init */
/* Not called anywhere yet (Monitor only shows the model). Kept for a
 * future CPU-usage readout. */
void cpu_metrics_init(void);
void cpu_metrics_update(void);
uint32_t cpu_usage_percent(void);
const char *cpu_model_str(void);

#endif
