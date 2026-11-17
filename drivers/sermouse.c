#include "drivers/sermouse.h"
#include "drivers/mouse.h"
#include "interrupts/idt.h"
#include "lib/io.h"
#include "lib/time.h"

/* 8250/16450/16550 registers (offsets from the port base) */
#define RBR 0 /* receive buffer (DLAB=0)  */
#define DLL 0 /* divisor low (DLAB=1)     */
#define IER 1 /* interrupt enable         */
#define DLM 1 /* divisor high (DLAB=1)    */
#define IIR 2 /* interrupt identification */
#define FCR 2 /* FIFO control (write)     */
#define LCR 3
#define MCR 4
#define LSR 5
#define SCR 7 /* scratch                  */

#define MCR_DTR 0x01
#define MCR_RTS 0x02
#define MCR_OUT2 0x08 /* gates the UART IRQ onto the bus on PCs */

static uint16_t s_base;
static int s_irq = -1, s_port;
static bool s_found;

/* interrupt -> poll ring buffer */
#define RING 64
static volatile uint8_t s_ring[RING];
static volatile uint8_t s_head, s_tail;

static uint8_t s_pkt[3];
static int s_idx;
static bool s_after_packet; /* a Logitech 4th byte may follow */

extern void isr_sermouse(void);

/* A UART answers at this base? (the scratch register keeps a value; on an
 * 8250 without scratch fall back to LCR read-back) */
static bool uart_present(uint16_t base) {
    if (inb(base + LSR) == 0xFF)
        return false;
    outb(base + SCR, 0x5A);
    if (inb(base + SCR) == 0x5A) {
        outb(base + SCR, 0xA5);
        if (inb(base + SCR) == 0xA5)
            return true;
    }
    uint8_t old = inb(base + LCR);
    outb(base + LCR, 0x1B);
    bool ok = inb(base + LCR) == 0x1B;
    outb(base + LCR, old);
    return ok;
}

static void uart_setup_1200_7n1(uint16_t base) {
    outb(base + IER, 0x00);
    outb(base + LCR, 0x80); /* DLAB */
    outb(base + DLL, 96);   /* 115200 / 1200 */
    outb(base + DLM, 0);
    outb(base + LCR, 0x02); /* 7 data bits, no parity, 1 stop */
    outb(base + FCR, 0x07); /* enable + clear FIFOs (ignored by 8250) */
}

/* Power-cycle the mouse through DTR/RTS and look for its 'M' banner. */
static bool probe(uint16_t base) {
    if (!uart_present(base))
        return false;
    uart_setup_1200_7n1(base);
    outb(base + MCR, 0x00); /* mouse off */
    sleep_ms(150);
    while (inb(base + LSR) & 0x01)
        (void)inb(base + RBR);                      /* drain */
    outb(base + MCR, MCR_DTR | MCR_RTS | MCR_OUT2); /* on */
    uint32_t start = time_millis();
    while (time_millis() - start < 300) {
        if (inb(base + LSR) & 0x01) {
            uint8_t c = inb(base + RBR) & 0x7F;
            if (c == 'M')
                return true;
        }
    }
    return false;
}

bool sermouse_init(void) {
    static const uint16_t bases[2] = {0x3F8, 0x2F8};
    static const int irqs[2] = {4, 3};
    for (int i = 0; i < 2 && !s_found; i++)
        if (probe(bases[i])) {
            s_base = bases[i];
            s_irq = irqs[i];
            s_port = i + 1;
            s_found = true;
        }
    if (!s_found)
        return false;
    /* swallow the rest of the banner ("3", "Z", ...) */
    sleep_ms(30);
    while (inb(s_base + LSR) & 0x01)
        (void)inb(s_base + RBR);
    s_head = s_tail = 0;
    s_idx = 0;
    outb(s_base + IER, 0x01); /* received-data interrupt */
    idt_set_irq(s_irq, isr_sermouse);
    return true;
}

bool sermouse_detected(void) { return s_found; }
int sermouse_port(void) { return s_port; }

void sermouse_irq_handler(void) {
    if (s_found) {
        /* drain everything (16550 FIFO may hold several bytes) */
        for (int n = 0; n < 16 && (inb(s_base + LSR) & 0x01); n++) {
            uint8_t c = inb(s_base + RBR);
            uint8_t next = (uint8_t)((s_head + 1) % RING);
            if (next != s_tail) { /* full: drop */
                s_ring[s_head] = c;
                s_head = next;
            }
        }
        (void)inb(s_base + IIR);
    }
    outb(0x20, 0x20); /* COM1/COM2 are on the master PIC */
}

/* Decode one byte of the Microsoft / Logitech protocol. */
static void feed(uint8_t c) {
    c &= 0x7F;
    if (c & 0x40) { /* sync bit: first byte of a packet */
        s_pkt[0] = c;
        s_idx = 1;
        s_after_packet = false;
        return;
    }
    if (s_idx == 0) {
        /* Logitech extension: a 4th byte right after a packet, bit 5 =
         * middle. Anything else here is noise while out of sync. */
        if (s_after_packet)
            mouse_apply(0, 0, mouse_live()->left, mouse_live()->right,
                        (c & 0x20) != 0);
        s_after_packet = false;
        return;
    }
    s_pkt[s_idx++] = c;
    if (s_idx < 3)
        return;
    s_idx = 0;
    s_after_packet = true;
    int8_t dx = (int8_t)(((s_pkt[0] & 0x03) << 6) | (s_pkt[1] & 0x3F));
    int8_t dy = (int8_t)(((s_pkt[0] & 0x0C) << 4) | (s_pkt[2] & 0x3F));
    /* Y grows downwards on the wire, like the screen */
    mouse_apply(dx, dy, (s_pkt[0] & 0x20) != 0, (s_pkt[0] & 0x10) != 0,
                mouse_live()->middle);
}

void sermouse_poll(void) {
    while (s_tail != s_head) {
        uint8_t c = s_ring[s_tail];
        s_tail = (uint8_t)((s_tail + 1) % RING);
        feed(c);
    }
}
