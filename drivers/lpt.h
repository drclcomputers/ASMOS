#ifndef LPT_H
#define LPT_H

#include "lib/core.h"

/*
 * Parallel ports (LPT1-3), standard (SPP) mode, polled.
 * Port addresses come from the BIOS data area (0x408), falling back to
 * probing 0x378 / 0x278 / 0x3BC.
 */

#define LPT_MAX 3

typedef enum {
    LPT_READY = 0,
    LPT_BUSY,    /* printer did not take the byte in time */
    LPT_OFFLINE, /* not selected / switched off */
    LPT_PAPER_OUT,
    LPT_ERROR, /* printer signals a fault */
    LPT_NO_PORT,
} lpt_status_t;

int lpt_init(void); /* returns number of ports found */
int lpt_count(void);
uint16_t lpt_base(int n); /* n = 0 for LPT1; 0 = absent */
lpt_status_t lpt_status(int n);
const char *lpt_status_text(lpt_status_t st);

/* Send raw bytes; stops at the first byte the printer refuses. Returns
 * bytes sent; *st (optional) receives the final status. */
uint32_t lpt_write(int n, const void *data, uint32_t len, lpt_status_t *st);

/* One byte if the printer is ready now (no waiting): LPT_READY = sent,
 * LPT_BUSY = try later, anything else = the printer's problem. */
lpt_status_t lpt_try_put(int n, uint8_t c);

/* Text helper: LF -> CR LF, ends with a form feed (eject the page). */
uint32_t lpt_print_text(int n, const char *text, uint32_t len,
                        lpt_status_t *st);

#endif
