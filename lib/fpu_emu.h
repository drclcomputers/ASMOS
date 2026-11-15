#ifndef FPU_EMU_H
#define FPU_EMU_H

/*
 * x87 emulator for machines without a maths coprocessor (386SX/DX, 486SX),
 * like Windows 3.1's WIN87EM. With CR0.EM set every x87 instruction raises
 * #NM; the handler decodes it, does the work in software and steps over it.
 *
 * Numbers are kept at the x87's own precision (64-bit mantissa, 15-bit
 * exponent), so results match a real FPU. Covered: loads/stores of all
 * formats (16/32/64-bit integers, float, double, 80-bit, BCD), arithmetic,
 * compares, control/status/environment (fnstsw, fldcw, fnstenv, fsave...),
 * fsqrt, fprem, frndint, fscale, fxtract and the transcendentals
 * (fsin, fcos, fsincos, fptan, fpatan, f2xm1, fyl2x, fyl2xp1). Exceptions
 * set their status flags but never trap (they are masked by default).
 *
 * It is slow (hundreds to thousands of cycles per instruction), as it was
 * on the real machines.
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
