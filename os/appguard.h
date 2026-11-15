#ifndef APPGUARD_H
#define APPGUARD_H

#include "lib/core.h"

typedef struct {
    uint32_t vector; /* 0=#DE 6=#UD 13=#GP 14=#PF */
    uint32_t error;
    uint32_t eip;
    uint32_t cr2;
} appfault_t;

/* Call fn(a, b) with fault containment. Returns true if it returned
 * normally; false if the CPU raised #DE/#UD/#GP/#PF inside it (details in
 * *out).  Nests safely. */
bool appguard_call2(void (*fn)(void *, void *), void *a, void *b,
                    appfault_t *out);

/* Not CPU vectors: why appguard_abort ended a call. */
#define APPFAULT_KILLED 0x100  /* the user ended it (not responding) */
#define APPFAULT_STOPPED 0x101 /* stopped for Monitor; app suspended */

/* A guarded call is running in the current task. */
bool appguard_active(void);
/* Timer IRQ only: abort that call (returns false if there is none). */
bool appguard_abort(uint32_t vector);

/* Install IDT gates for all CPU exception vectors 0..31. */
void appguard_install_gates(void (*set_gate)(uint8_t, uint32_t));

#endif
