#include "drivers/ne2000.h"
#include "drivers/pci.h"
#include "interrupts/idt.h"
#include "lib/core.h"
#include "lib/io.h"
#include "lib/memory.h"
#include "shell/term_buf.h"

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t next_page;
    uint16_t byte_count;
} rx_header_t;

uint16_t g_ne_base = 0x300;
static int s_irq = -1;
static bool s_pci;
static bool s_detected = false;
static uint8_t s_mac[6];
static uint8_t s_curr;
static ne2000_rx_cb s_rx_cb = NULL;

static uint8_t *s_rx_buf = NULL;
#define RX_BUF_SIZE 1518

static uint8_t s_pad_buf[60];

/* The NIC's remote-DMA registers are shared state: an RX interrupt in the
 * middle of a TX copy corrupts both. Every multi-step NIC access runs with
 * interrupts off; save/restore so we never re-enable them by accident. */
static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f)::"memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    __asm__ volatile("pushl %0; popfl" ::"r"(f) : "memory", "cc");
}

static void ne_write_rdc(uint16_t addr, uint16_t count) {
    outb(P0_RSAR0, addr & 0xFF);
    outb(P0_RSAR1, addr >> 8);
    outb(P0_RBCR0, count & 0xFF);
    outb(P0_RBCR1, count >> 8);
}

static void ne_remote_read(uint16_t src, uint8_t *dst, uint16_t len) {
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    ne_write_rdc(src, len);
    outb(P0_CMD, CMD_RD0 | CMD_PS0 | CMD_START);
    for (uint16_t i = 0; i < len; i++)
        dst[i] = inb(NE_DATA);
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
}

static void ne_remote_write(uint16_t dst, const uint8_t *src, uint16_t len) {
    outb(P0_ISR, ISR_RDC);
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    ne_write_rdc(dst, len);
    outb(P0_CMD, CMD_RD1 | CMD_PS0 | CMD_START);
    for (uint16_t i = 0; i < len; i++)
        outb(NE_DATA, src[i]);
    uint32_t t = 0x10000;
    while (!(inb(P0_ISR) & ISR_RDC) && t--)
        ;
    outb(P0_ISR, ISR_RDC);
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
}

/* Reset the card at g_ne_base and read its station address PROM. True if
 * an 8390/NE2000 answered with a plausible MAC. */
static bool probe_at(uint16_t base) {
    g_ne_base = base;
    uint8_t cmd = inb(P0_CMD);
    if (cmd == 0xFF)
        return false; /* nothing decodes this port */
    outb(NE_RESET_PORT, inb(NE_RESET_PORT));
    uint32_t t = 0x20000;
    while (!(inb(P0_ISR) & ISR_RST) && --t)
        ;
    if (!t)
        return false; /* no 8390 reset acknowledge */
    outb(P0_ISR, 0xFF);

    outb(P0_CMD, CMD_STOP | CMD_RD2);
    outb(P0_DCR, DCR_FT1 | DCR_LS);
    outb(P0_RBCR0, 0);
    outb(P0_RBCR1, 0);
    outb(P0_RCR, RCR_MON);
    outb(P0_TCR, TCR_LB0);
    outb(P0_TPSR, TX_PAGE);
    outb(P0_PSTART, RX_START);
    outb(P0_PSTOP, RX_STOP);
    outb(P0_BNRY, RX_START);
    outb(P0_ISR, 0xFF);
    outb(P0_IMR, 0x00);

    uint8_t prom[32];
    ne_remote_read(0, prom, 32);
    bool all_ff = true, all_00 = true;
    for (int i = 0; i < 6; i++) {
        s_mac[i] = prom[i * 2];
        all_ff &= s_mac[i] == 0xFF;
        all_00 &= s_mac[i] == 0x00;
    }
    return !all_ff && !all_00 && !(s_mac[0] & 1); /* unicast address */
}

/* ISA card with an unknown jumper: make it raise "remote DMA complete" and
 * see which PIC request line goes up. -1 if none did. */
static int detect_isa_irq(void) {
    uint32_t fl = irq_save();
    outb(0x20, 0x0A); /* OCW3: read IRR */
    outb(0xA0, 0x0A);
    uint16_t before = inb(0x20) | (inb(0xA0) << 8);
    outb(P0_ISR, 0xFF);
    outb(P0_IMR, ISR_RDC);
    /* 2-byte remote write; completion sets RDC and raises the IRQ line. The
     * line must still be high when IRR is read (the 8259 forgets edges that
     * drop before they are acknowledged), so ISR is cleared afterwards. */
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    ne_write_rdc(0x4000, 2);
    outb(P0_CMD, CMD_RD1 | CMD_PS0 | CMD_START);
    outb(NE_DATA, 0);
    outb(NE_DATA, 0);
    uint32_t t = 0x10000;
    while (!(inb(P0_ISR) & ISR_RDC) && --t)
        ;
    uint16_t after = inb(0x20) | (inb(0xA0) << 8);
    outb(P0_IMR, 0x00);
    outb(P0_ISR, 0xFF);
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    irq_restore(fl);
    if (!t)
        return -1;
    uint16_t rose =
        (uint16_t)(after & ~before) & ~(uint16_t)0x0007; /* not 0-2 */
    for (int irq = 3; irq < 16; irq++)
        if (rose == (1u << irq))
            return irq; /* exactly one line: trust it */
    return -1;
}

extern void isr_ne2000(void);

bool ne2000_init(void) {
    s_rx_buf = (uint8_t *)kmalloc(RX_BUF_SIZE);
    if (!s_rx_buf)
        return false;

    bool found = false;
    /* PCI NE2000 clones: Realtek RTL8029, Winbond W89C940, VIA, Compex... */
    static const uint16_t ids[][2] = {
        {0x10EC, 0x8029}, {0x1050, 0x0940}, {0x1050, 0x5A5A}, {0x11F6, 0x1401},
        {0x8E2E, 0x3000}, {0x4A14, 0x5000}, {0x1106, 0x0926}, {0x10BD, 0x0E34}};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]) && !found; i++) {
        const pci_dev_t *d = pci_find(ids[i][0], ids[i][1], 0);
        uint16_t io = pci_bar_io(d, 0);
        if (!io)
            continue;
        pci_enable(d, true, false, false);
        if (probe_at(io)) {
            found = s_pci = true;
            s_irq = d->irq != 0xFF ? d->irq : -1;
        }
    }
    static const uint16_t isa[] = {0x300, 0x280, 0x320, 0x340, 0x360, 0x240};
    for (unsigned i = 0; i < sizeof(isa) / sizeof(isa[0]) && !found; i++)
        if (probe_at(isa[i])) {
            found = true;
            s_irq = detect_isa_irq();
        }
    if (!found) {
        kfree(s_rx_buf);
        s_rx_buf = NULL;
        return false;
    }

    outb(P0_CMD, CMD_STOP | CMD_RD2 | CMD_PS1);
    for (int i = 0; i < 6; i++)
        outb(P1_PAR0 + i, s_mac[i]);
    for (int i = 0; i < 8; i++)
        outb(P1_MAR0 + i, 0xFF);
    s_curr = RX_START + 1;
    outb(P1_CURR, s_curr);

    outb(P0_CMD, CMD_START | CMD_RD2 | CMD_PS0);
    outb(P0_DCR, DCR_FT1 | DCR_LS);
    outb(P0_RBCR0, 0);
    outb(P0_RBCR1, 0);
    outb(P0_RCR, RCR_AB);
    outb(P0_TCR, 0x00);
    outb(P0_TPSR, TX_PAGE);
    outb(P0_PSTART, RX_START);
    outb(P0_PSTOP, RX_STOP);
    outb(P0_BNRY, RX_START);
    outb(P0_ISR, 0xFF);

    s_detected = true;
    /* Interrupts only when we know the line; otherwise net_poll() drives
     * the card (it already polls every frame). */
    if (s_irq >= 3 && s_irq <= 15 && s_irq != 8 && !idt_irq_in_use(s_irq)) {
        outb(P0_IMR, ISR_PRX | ISR_RXE | ISR_OVW);
        idt_set_irq(s_irq, isr_ne2000);
    } else {
        s_irq = -1;
        outb(P0_IMR, 0x00);
    }
    return true;
}

uint16_t ne2000_io_base(void) { return g_ne_base; }
int ne2000_irq(void) { return s_irq; }
const char *ne2000_bus(void) { return s_pci ? "PCI" : "ISA"; }

bool ne2000_detected(void) { return s_detected; }

void ne2000_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++)
        mac[i] = s_mac[i];
}

bool ne2000_send(const uint8_t *data, uint16_t len) {
    if (!s_detected || len > 1518)
        return false;

    if (len < 60) {
        memcpy(s_pad_buf, data, len);
        memset(s_pad_buf + len, 0, 60 - len);
        data = s_pad_buf;
        len = 60;
    }

    uint32_t fl = irq_save();
    ne_remote_write(TX_PAGE * 256, data, len);
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    outb(P0_TPSR, TX_PAGE);
    outb(P0_TBCR0, len & 0xFF);
    outb(P0_TBCR1, len >> 8);
    outb(P0_CMD, CMD_RD2 | CMD_TXP | CMD_PS0 | CMD_START);

    uint32_t t = 0x100000;
    while ((inb(P0_CMD) & CMD_TXP) && t)
        t--; /* (old `&& t--` wrapped to 0xFFFFFFFF, so the check never hit) */
    uint8_t tsr = inb(P0_TSR);
    irq_restore(fl);
    if (t == 0) {
        term_buf_push("NE2000: TX timeout");
        return false;
    }
    if (tsr & 0x02)
        term_buf_push("NE2000: TX abort");
    return true;
}

void ne2000_set_rx_callback(ne2000_rx_cb cb) { s_rx_cb = cb; }

static bool ne_receive_one(void) {
    outb(P0_CMD, CMD_RD2 | CMD_PS1 | CMD_START);
    uint8_t curr = inb(P1_CURR);
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    uint8_t bnry = inb(P0_BNRY);

    uint8_t next_page = bnry + 1;
    if (next_page >= RX_STOP)
        next_page = RX_START;

    if (next_page == curr)
        return false;

    rx_header_t hdr;
    ne_remote_read(next_page * 256, (uint8_t *)&hdr, sizeof(hdr));

    if (hdr.byte_count < sizeof(rx_header_t) ||
        hdr.byte_count > RX_BUF_SIZE + sizeof(rx_header_t) ||
        hdr.next_page < RX_START || hdr.next_page >= RX_STOP) {
        /* Corrupt header: drop everything pending (BNRY = CURR - 1). */
        outb(P0_BNRY, curr == RX_START ? RX_STOP - 1 : curr - 1);
        return false;
    }

    uint16_t pkt_len = hdr.byte_count - sizeof(rx_header_t);
    if (pkt_len > 0 && s_rx_cb && s_rx_buf) {
        uint16_t src_addr = next_page * 256 + sizeof(rx_header_t);
        if (src_addr + pkt_len > RX_STOP * 256) {
            uint16_t first = (uint16_t)(RX_STOP * 256 - src_addr);
            ne_remote_read(src_addr, s_rx_buf, first);
            ne_remote_read(RX_START * 256, s_rx_buf + first, pkt_len - first);
        } else {
            ne_remote_read(src_addr, s_rx_buf, pkt_len);
        }
        s_rx_cb(s_rx_buf, pkt_len);
    }

    uint8_t new_bnry = hdr.next_page - 1;
    if (new_bnry < RX_START)
        new_bnry = RX_STOP - 1;
    outb(P0_BNRY, new_bnry);
    return true;
}

static inline void eoi(void) {
    if (s_irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void ne2000_irq_handler(void) {
    if (!s_detected) {
        eoi();
        return;
    }

    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    uint8_t isr = inb(P0_ISR);

    if (isr & ISR_OVW) {
        outb(P0_CMD, CMD_STOP | CMD_RD2);
        for (volatile int i = 0; i < 0x2000; i++)
            __asm__ volatile("nop");
        outb(P0_RBCR0, 0);
        outb(P0_RBCR1, 0);
        outb(P0_TCR, TCR_LB0);
        outb(P0_CMD, CMD_START | CMD_RD2);
        ne_receive_one();
        outb(P0_ISR, ISR_OVW);
        outb(P0_TCR, 0x00);
    }

    if (isr & (ISR_PRX | ISR_RXE)) {
        outb(P0_ISR, ISR_PRX | ISR_RXE);
        while (ne_receive_one()) /* drain all; one IRQ can mean many frames */
            ;
    }

    eoi();
}

void ne2000_poll(void) {
    if (!s_detected)
        return;
    uint32_t fl = irq_save();
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    uint8_t isr = inb(P0_ISR);

    if (isr & ISR_OVW) {
        outb(P0_CMD, CMD_STOP | CMD_RD2);
        for (volatile int i = 0; i < 0x2000; i++)
            __asm__ volatile("nop");
        outb(P0_RBCR0, 0);
        outb(P0_RBCR1, 0);
        outb(P0_TCR, TCR_LB0);
        outb(P0_CMD, CMD_START | CMD_RD2);
        ne_receive_one();
        outb(P0_ISR, ISR_OVW);
        outb(P0_TCR, 0x00);
    }

    if (isr & (ISR_PRX | ISR_RXE)) {
        outb(P0_ISR, ISR_PRX | ISR_RXE);
        while (ne_receive_one())
            ;
    }

    irq_restore(fl);
}
