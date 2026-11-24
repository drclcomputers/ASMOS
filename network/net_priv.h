// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef NET_PRIV_H
#define NET_PRIV_H

#include "network/net.h"

extern net_socket_t *s_socks;
extern uint8_t s_ip[4];
extern uint8_t s_rx_mac[6];

void ip_send(const uint8_t dst_mac[6], const uint8_t dst_ip[4], uint8_t proto,
             const uint8_t *payload, uint16_t plen);
bool resolve_mac(const uint8_t ip[4], uint8_t mac_out[6]);
uint16_t udp_tcp_checksum(const uint8_t src[4], const uint8_t dst[4],
                          uint8_t proto, const uint8_t *data, uint16_t len);

void tcp_input(const ip_hdr_t *iph, const uint8_t *data, uint16_t len);
void tcp_tick(void); /* timers: from net_poll */

#endif
