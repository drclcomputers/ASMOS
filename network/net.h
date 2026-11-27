// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef NET_H
#define NET_H

#include "lib/core.h"

#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_ARP 0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

#define ICMP_ECHO_REQ 8
#define ICMP_ECHO_REP 0

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2
#define ARP_CACHE_SIZE 16

#define NET_MAX_SOCKETS 32
#define NET_UDP_BUFSIZE 1460
#define NET_TCP_BUFSIZE 4096

#define RX_QUEUE_SIZE 32
#define RX_PKT_MAX 1518

typedef struct {
    uint8_t data[RX_PKT_MAX];
    uint16_t len;
} rx_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t htype_hi, htype_lo;
    uint8_t ptype_hi, ptype_lo;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} arp_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t ihl_ver;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint8_t src[4];
    uint8_t dst[4];
} ip_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_off;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

typedef enum {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_LISTEN,
    TCP_SYN_RCVD,
    TCP_CLOSING
} tcp_state_t;

/* per connection (network/tcp.c): what is sent and not yet acknowledged,
 * and what came and is not yet read */
#define TCP_TXBUF 8192
#define TCP_RXBUF 8192 /* (the window offered) */
#define TCP_MSS 1460

typedef struct {
    bool used;
    uint8_t proto;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t remote_ip[4];
    uint8_t remote_mac[6];

    uint8_t *rx_buf; /* UDP: one datagram */
    uint16_t rx_len;
    bool rx_ready;

    tcp_state_t tcp_state;
    struct tcp_cb *tcb; /* TCP: network/tcp.c */
} net_socket_t;

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    bool valid;
} arp_entry_t;

void net_init(void);
void net_set_ip(const uint8_t ip[4], const uint8_t gw[4], const uint8_t nm[4]);
void net_get_ip(uint8_t ip[4]);
void net_get_config(uint8_t ip[4], uint8_t gw[4], uint8_t nm[4]);
bool net_ready(void); /* a card and an address (DHCP may not have one yet) */
/* one UDP datagram to a known MAC, without a socket or ARP (DHCP) */
bool net_send_udp(const uint8_t mac[6], const uint8_t dst_ip[4],
                  uint16_t src_port, uint16_t dst_port, const uint8_t *data,
                  uint16_t len);
void net_rx_enqueue(const uint8_t *frame, uint16_t len);

void arp_request(const uint8_t ip[4]);
bool arp_lookup(const uint8_t ip[4], uint8_t mac_out[6]);
void arp_announce(void);

int udp_open(uint16_t local_port);
void udp_close(int sock);
bool udp_send(int sock, const uint8_t dst_ip[4], uint16_t dst_port,
              const uint8_t *data, uint16_t len);
int udp_recv(int sock, uint8_t *buf, uint16_t maxlen, uint8_t src_ip[4],
             uint16_t *src_port);

/* TCP. Lost data is sent again until the other side has it; sockets
 * are numbers, -1 = none. */
/* waits until connected (5 s); local_port 0 = any */
int tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                uint16_t local_port);
/* doesn't wait: SYN sent, the socket in TCP_SYN_SENT; tcp_state says when
 * it is TCP_ESTABLISHED (or tcp_was_reset: refused). Give up with
 * tcp_abort. (The address's MAC: ARP, which may wait a moment.) */
int tcp_open(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t local_port);
/* closes and waits (3 s) for the rest to be acknowledged */
void tcp_close(int sock);
/* closes and returns: the stack finishes the closing in the background */
void tcp_release(int sock);
/* cuts it off (RST): what wasn't sent or read is lost */
void tcp_abort(int sock);
/* waits while the send buffer is full; false = the connection is gone */
bool tcp_send(int sock, const uint8_t *data, uint16_t len);
/* doesn't wait: the bytes taken (0 = the buffer is full), -1 = gone */
int tcp_write(int sock, const uint8_t *data, uint16_t len);
int tcp_recv(int sock, uint8_t *buf, uint16_t maxlen); /* 0 = nothing yet */
uint16_t tcp_available(int sock); /* bytes waiting to be read */
uint16_t tcp_pending(int sock);   /* bytes not yet acknowledged */
tcp_state_t tcp_state(int sock);
bool tcp_was_reset(int sock); /* refused, reset, or gave up resending */
bool tcp_peer(int sock, uint8_t ip[4], uint16_t *port);
uint16_t tcp_local_port(int sock);
/* a server: connections come in on the port, tcp_accept takes them (it
 * doesn't wait). Closing the listener refuses new ones. */
int tcp_listen(uint16_t port);
int tcp_accept(int listen_sock);

typedef struct {
    uint32_t segs_in, segs_out;
    uint32_t timeouts;    /* sent again when the timer ran out */
    uint32_t fast_rexmit; /* sent again on duplicate ACKs */
} tcp_stats_t;
extern tcp_stats_t g_tcp_stats;

bool icmp_ping(const uint8_t dst_ip[4], uint16_t id, uint16_t seq,
               uint32_t timeout_ms);

void net_poll(void);

uint16_t net_checksum(const void *data, uint16_t len);
uint16_t net_htons(uint16_t v);
uint32_t net_htonl(uint32_t v);
#define net_ntohs(v) net_htons(v)
#define net_ntohl(v) net_htonl(v)

#endif
