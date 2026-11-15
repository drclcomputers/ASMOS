#include "drivers/lpt.h"
#include "lib/io.h"

/* registers: base+0 data, base+1 status, base+2 control */
#define ST_NERROR 0x08 /* 0 = error          */
#define ST_SELECT 0x10 /* 1 = online         */
#define ST_PAPEROUT 0x20
#define ST_NACK 0x40
#define ST_NBUSY 0x80 /* 1 = ready (inverted) */

#define CT_STROBE 0x01 /* inverted on the cable */
#define CT_NINIT 0x04  /* 0 = reset printer  */
#define CT_SELECTIN 0x08

static uint16_t s_base[LPT_MAX];
static int s_count;

static void io_delay(int n) {
    while (n--)
        inb(0x80); /* ~1 us each on ISA */
}

static bool port_present(uint16_t base) {
    outb(base, 0x55);
    io_delay(2);
    if (inb(base) != 0x55)
        return false;
    outb(base, 0xAA);
    io_delay(2);
    return inb(base) == 0xAA;
}

int lpt_init(void) {
    s_count = 0;
    /* BIOS data area: up to 3 LPT bases at 0x408 */
    for (int i = 0; i < LPT_MAX; i++) {
        uint16_t b = bios_data_word(8 + i * 2);
        if (b && port_present(b))
            s_base[s_count++] = b;
    }
    if (!s_count) {
        static const uint16_t probe[3] = {0x378, 0x278, 0x3BC};
        for (int i = 0; i < 3; i++)
            if (port_present(probe[i]))
                s_base[s_count++] = probe[i];
    }
    for (int i = 0; i < s_count; i++) {
        /* reset the printer: pulse nINIT low, then select it */
        outb(s_base[i] + 2, CT_SELECTIN);
        io_delay(60);
        outb(s_base[i] + 2, CT_SELECTIN | CT_NINIT);
    }
    return s_count;
}

int lpt_count(void) { return s_count; }
uint16_t lpt_base(int n) { return (n >= 0 && n < s_count) ? s_base[n] : 0; }

static lpt_status_t decode(uint8_t st) {
    if (st == 0xFF)
        return LPT_OFFLINE; /* floating: nothing on the cable */
    if (st & ST_PAPEROUT)
        return LPT_PAPER_OUT;
    if (!(st & ST_SELECT))
        return LPT_OFFLINE;
    if (!(st & ST_NERROR))
        return LPT_ERROR;
    if (!(st & ST_NBUSY))
        return LPT_BUSY;
    return LPT_READY;
}

lpt_status_t lpt_status(int n) {
    uint16_t b = lpt_base(n);
    return b ? decode(inb(b + 1)) : LPT_NO_PORT;
}

const char *lpt_status_text(lpt_status_t st) {
    switch (st) {
    case LPT_READY:
        return "ready";
    case LPT_BUSY:
        return "busy (timed out)";
    case LPT_OFFLINE:
        return "offline or not connected";
    case LPT_PAPER_OUT:
        return "out of paper";
    case LPT_ERROR:
        return "printer error";
    default:
        return "no such port";
    }
}

/* One byte with the Centronics handshake. Printers can stay busy for a
 * while (line feed, head return), so the wait is generous: ~2 s. */
static lpt_status_t put_byte(uint16_t b, uint8_t c) {
    uint32_t t = 2000000;
    uint8_t st;
    while (!((st = inb(b + 1)) & ST_NBUSY)) {
        if (!--t)
            return LPT_BUSY;
        if ((st & ST_PAPEROUT) || !(st & ST_SELECT) || !(st & ST_NERROR))
            return decode(st);
    }
    lpt_status_t s = decode(st);
    if (s != LPT_READY)
        return s;
    outb(b, c);
    io_delay(1);                                     /* data setup */
    outb(b + 2, CT_SELECTIN | CT_NINIT | CT_STROBE); /* strobe on  */
    io_delay(1);
    outb(b + 2, CT_SELECTIN | CT_NINIT); /* strobe off */
    io_delay(1);
    return LPT_READY;
}

uint32_t lpt_write(int n, const void *data, uint32_t len, lpt_status_t *st) {
    uint16_t b = lpt_base(n);
    lpt_status_t s = b ? LPT_READY : LPT_NO_PORT;
    uint32_t i = 0;
    const uint8_t *p = (const uint8_t *)data;
    for (; b && i < len; i++)
        if ((s = put_byte(b, p[i])) != LPT_READY)
            break;
    if (st)
        *st = s;
    return i;
}

uint32_t lpt_print_text(int n, const char *text, uint32_t len,
                        lpt_status_t *st) {
    uint16_t b = lpt_base(n);
    lpt_status_t s = b ? LPT_READY : LPT_NO_PORT;
    uint32_t i = 0;
    for (; b && i < len; i++) {
        char c = text[i];
        if (c == '\r')
            continue;
        if (c == '\n' && (s = put_byte(b, '\r')) != LPT_READY)
            break;
        if ((s = put_byte(b, (uint8_t)c)) != LPT_READY)
            break;
    }
    if (s == LPT_READY && b)
        s = put_byte(b, '\f'); /* eject the page */
    if (st)
        *st = s;
    return i;
}
