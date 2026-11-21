#ifndef FPU_EMU_H
#define FPU_EMU_H

/*
 * Software x87 for 386/486 without an FPU. #NM handlers emulate loads/stores
 * (integer, float, double, 80-bit, BCD), arithmetic, comparisons, and
 * transcendental operations at x87 precision. Masked exceptions set flags.
 */

#include "lib/core.h"

/* registers as the #NM stub saves them (pusha order) + the CPU's frame */
typedef struct {
    uint32_t edi, esi, ebp, esp_at_pusha, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags;
} fpu_trap_frame_t;

/* Install the #NM handler (call when there is no FPU and CR0.EM is set). */
void fpu_emu_install(void (*set_gate)(uint8_t, uint32_t));
/* The C side of the trap: true if the instruction was emulated. */
bool fpu_emu_trap(fpu_trap_frame_t *f);
/* Instructions emulated so far (Monitor / shell `cpu`). */
uint32_t fpu_emu_count(void);

#endif
