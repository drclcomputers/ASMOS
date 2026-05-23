#include "drivers/ne2000.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "shell/term_buf.h"

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t next_page;
    uint16_t byte_count;
} rx_header_t;

static bool s_detected = false;
static uint8_t s_mac[6];
static uint8_t s_curr;
static ne2000_rx_cb s_rx_cb = NULL;

static uint8_t *s_rx_buf = NULL;
#define RX_BUF_SIZE 1518

static uint8_t s_pad_buf[60];

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

bool ne2000_init(void) {
    s_rx_buf = (uint8_t *)kmalloc(RX_BUF_SIZE);
    if (!s_rx_buf)
        return false;

    outb(NE_RESET_PORT, inb(NE_RESET_PORT));
    for (volatile int i = 0; i < 0x8000; i++)
        __asm__ volatile("nop");

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

    bool valid = false;
    for (int i = 0; i < 6; i++) {
        s_mac[i] = prom[i * 2];
        if (s_mac[i] != 0xFF)
            valid = true;
    }
    if (!valid) {
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
    outb(P0_IMR, ISR_PRX | ISR_RXE | ISR_OVW);

    s_detected = true;
    return true;
}

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

    ne_remote_write(TX_PAGE * 256, data, len);
    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    outb(P0_TPSR, TX_PAGE);
    outb(P0_TBCR0, len & 0xFF);
    outb(P0_TBCR1, len >> 8);
    outb(P0_CMD, CMD_RD2 | CMD_TXP | CMD_PS0 | CMD_START);

    uint32_t t = 0x100000;
    while ((inb(P0_CMD) & CMD_TXP) && t--)
        ;
    if (t == 0) {
        term_buf_push("NE2000: TX timeout");
        return false;
    }
    uint8_t tsr = inb(P0_TSR);
    if (tsr & 0x01)
        term_buf_push("NE2000: TX collision");
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
        hdr.byte_count > RX_BUF_SIZE + sizeof(rx_header_t)) {
        outb(P0_BNRY, next_page);
        return true;
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

void ne2000_irq_handler(void) {
    if (!s_detected) {
        outb(0x20, 0x20);
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
        ne_receive_one();
    }

    outb(0x20, 0x20);
}

void ne2000_poll(void) {
    if (!s_detected)
        return;
    __asm__ volatile("cli");
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

    __asm__ volatile("sti");
}
