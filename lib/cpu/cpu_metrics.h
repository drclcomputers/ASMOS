#ifndef CPU_METRICS_H
#define CPU_METRICS_H

#include "lib/core.h"

void cpu_model_init(void);
void cpu_metrics_init(void);
void cpu_metrics_update(void);
uint32_t cpu_usage_percent(void);
const char *cpu_model_str(void);

#endif
