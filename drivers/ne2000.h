#ifndef NE2000_H
#define NE2000_H

#include "lib/core.h"

#define NE2000_IRQ 3
#define NE2000_VECTOR (32 + NE2000_IRQ)

#define NE_BASE 0x300
#define NE_RESET_PORT (NE_BASE + 0x1F)
#define NE_DATA (NE_BASE + 0x10)

#define P0_CMD (NE_BASE + 0x00)
#define P0_PSTART (NE_BASE + 0x01)
#define P0_PSTOP (NE_BASE + 0x02)
#define P0_BNRY (NE_BASE + 0x03)
#define P0_TSR (NE_BASE + 0x04)
#define P0_TPSR (NE_BASE + 0x04)
#define P0_TBCR0 (NE_BASE + 0x05)
#define P0_TBCR1 (NE_BASE + 0x06)
#define P0_ISR (NE_BASE + 0x07)
#define P0_RSAR0 (NE_BASE + 0x08)
#define P0_RSAR1 (NE_BASE + 0x09)
#define P0_RBCR0 (NE_BASE + 0x0A)
#define P0_RBCR1 (NE_BASE + 0x0B)
#define P0_RCR (NE_BASE + 0x0C)
#define P0_TCR (NE_BASE + 0x0D)
#define P0_DCR (NE_BASE + 0x0E)
#define P0_IMR (NE_BASE + 0x0F)
#define P1_CMD (NE_BASE + 0x00)
#define P1_PAR0 (NE_BASE + 0x01)
#define P1_CURR (NE_BASE + 0x07)
#define P1_MAR0 (NE_BASE + 0x08)

#define CMD_STOP 0x01
#define CMD_START 0x02
#define CMD_TXP 0x04
#define CMD_RD0 0x08
#define CMD_RD1 0x10
#define CMD_RD2 0x20
#define CMD_PS0 0x00
#define CMD_PS1 0x40
#define CMD_PS2 0x80

#define TX_PAGE 0x40
#define RX_START 0x48
#define RX_STOP 0x80

#define ISR_PRX 0x01
#define ISR_PTX 0x02
#define ISR_RXE 0x04
#define ISR_TXE 0x08
#define ISR_OVW 0x10
#define ISR_RDC 0x40
#define ISR_RST 0x80

#define DCR_WTS 0x01
#define DCR_BOS 0x02
#define DCR_LAS 0x04
#define DCR_LS 0x08
#define DCR_FT1 0x40

#define RCR_SEP 0x01
#define RCR_AR 0x02
#define RCR_AB 0x04
#define RCR_AM 0x08
#define RCR_PRO 0x10
#define RCR_MON 0x20

#define TCR_LB0 0x02

typedef void (*ne2000_rx_cb)(const uint8_t *data, uint16_t len);

bool ne2000_init(void);
bool ne2000_detected(void);
void ne2000_get_mac(uint8_t mac[6]);
bool ne2000_send(const uint8_t *data, uint16_t len);
void ne2000_set_rx_callback(ne2000_rx_cb cb);
void ne2000_irq_handler(void);
void ne2000_poll(void);

#endif
