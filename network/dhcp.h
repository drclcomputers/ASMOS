// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef DHCP_H
#define DHCP_H

#include "lib/core.h"

/*
 * DHCP client (RFC 2131): asks the network for an address, a netmask, a
 * gateway and a DNS server, and keeps the lease (renews at half of it).
 * Never waits: net_poll drives it (dhcp_tick), replies come in through
 * dhcp_input. While it has no address the stack's is 0.0.0.0.
 */

typedef enum {
    DHCP_OFF,        /* fixed address (Settings) */
    DHCP_SEARCHING,  /* DISCOVER sent, waiting for an OFFER */
    DHCP_REQUESTING, /* REQUEST sent for an offer */
    DHCP_BOUND,      /* has an address */
    DHCP_RENEWING,   /* half the lease gone: asking its server again */
    DHCP_REBINDING,  /* 7/8 gone: asking any server */
} dhcp_state_t;

typedef struct {
    dhcp_state_t state;
    uint8_t ip[4], netmask[4], gateway[4], dns[4], server[4];
    uint32_t lease_s;      /* the whole lease (0xFFFFFFFF: for ever) */
    uint32_t lease_left_s; /* what is left of it */
    int tries;             /* DISCOVERs sent without an answer */
} dhcp_info_t;

void dhcp_start(void);        /* get an address (again) */
void dhcp_stop(bool release); /* stop; release: give the address back */
dhcp_state_t dhcp_state(void);
void dhcp_info(dhcp_info_t *out);
const char *dhcp_state_name(dhcp_state_t s);

/* net.c: a DHCP packet from a server (UDP port 68), and the clock */
void dhcp_input(const uint8_t src_mac[6], const uint8_t *data, uint16_t len);
void dhcp_tick(void);

#endif
