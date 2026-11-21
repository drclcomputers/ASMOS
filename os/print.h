#ifndef OS_PRINT_H
#define OS_PRINT_H

#include "lib/core.h"

/*
 * Jobs are queued and print_pump() sends bytes to the parallel port without
 * blocking. PRN_TEXT supports plain text, PRN_ESCP Epson dot-matrix, and
 * PRN_PCL HP printers; graphics require the printer's own language.
 */

enum { PRN_TEXT, PRN_ESCP, PRN_PCL, PRN_TYPES };
extern const char *const PRN_TYPE_NAMES[PRN_TYPES];

/* page layout for text: 80 columns, 60 lines (11" at 6 lines an inch,
 * less margins); tabs every 8 */
#define PRINT_COLS 80
#define PRINT_LINES 60

enum { PJ_WAITING, PJ_PRINTING, PJ_ERROR };

typedef struct {
    int id;
    char name[28];
    uint8_t *data; /* printer bytes */
    uint32_t len, sent;
    int pages;
    uint8_t state; /* PJ_* */
    uint8_t port;  /* LPT index, from Settings when submitted */
} print_job_t;

#define PRINT_MAX_JOBS 8

/* Add a job; returns its id, or -1 (queue full, out of memory). The
 * printer type and port are those of Settings at the time. Text pages
 * `from`..`to` (1-based; 0, 0 = all). */
int print_submit_text(const char *name, const char *text, uint32_t len,
                      int copies, int from, int to);
/* an 8-bit picture of the 16 standard colours, `stride` bytes a row */
int print_submit_image(const char *name, const uint8_t *pix, int w, int h,
                       int stride, int copies);

void print_pump(void); /* send what the printer takes now */

int print_job_count(void);
const print_job_t *print_job(int i); /* 0 = the one printing */
bool print_delete(int id);
void print_set_paused(bool paused);
bool print_paused(void);
const char *print_status_text(void); /* "Printing", "Out of paper", ... */
const char *print_port_name(void);   /* "LPT1", or "no port" */
bool print_available(void);          /* a port exists */
uint32_t print_queue_rev(void);      /* changes whenever the queue does */

/* text layout, shared by printing and the preview */
int print_text_pages(const char *text, uint32_t len);
/* lines of page `page` (1-based) into lines[PRINT_LINES][PRINT_COLS + 1];
 * returns how many */
int print_text_page(const char *text, uint32_t len, int page,
                    char lines[][PRINT_COLS + 1]);

/* how a picture is placed on the page (for the preview): printer dots
 * across and down for one picture pixel, given the printer type */
void print_image_scale(int w, int h, int type, int *sx, int *sy);

/* grey level 0 (black) .. 255 (white) of one of the 16 colours */
uint8_t print_grey(uint8_t colour);
/* ordered 4x4 dither: ink at (x, y) for this grey? */
bool print_ink(uint8_t grey, int x, int y);

#endif
