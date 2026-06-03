#include "network/net.h"
#include "drivers/ne2000.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/time.h"
#include "os/scheduler.h"

static uint8_t s_ip[4] = {0};
static uint8_t s_gw[4] = {0};
static uint8_t s_nm[4] = {255, 255, 255, 0};
static uint8_t s_mac[6] = {0};

static const uint8_t MAC_BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t IP_BCAST[4] = {0xFF, 0xFF, 0xFF, 0xFF};

static volatile bool s_ping_reply = false;
static uint16_t s_ping_id = 0;
static uint16_t s_ping_seq = 0;

static net_socket_t *s_socks = NULL;
static rx_pkt_t *s_rx_queue = NULL;

static volatile uint8_t s_rx_head = 0;
static volatile uint8_t s_rx_tail = 0;

uint16_t net_htons(uint16_t v) { return (v >> 8) | (v << 8); }
uint32_t net_htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) |
           (v >> 24);
}

uint16_t net_checksum(const void *data, uint16_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint16_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static uint16_t udp_tcp_checksum(const uint8_t src[4], const uint8_t dst[4],
                                 uint8_t proto, const uint8_t *data,
                                 uint16_t len) {
    uint8_t pseudo[12];
    memcpy(pseudo, src, 4);
    memcpy(pseudo + 4, dst, 4);
    pseudo[8] = 0;
    pseudo[9] = proto;
    pseudo[10] = len >> 8;
    pseudo[11] = len & 0xFF;
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2)
        sum += (uint16_t)(pseudo[i] << 8 | pseudo[i + 1]);
    const uint8_t *p = data;
    uint16_t l = len;
    while (l > 1) {
        sum += (uint16_t)(p[0] << 8 | p[1]);
        p += 2;
        l -= 2;
    }
    if (l)
        sum += (uint16_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static bool ip_is_local(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++)
        if ((ip[i] & s_nm[i]) != (s_ip[i] & s_nm[i]))
            return false;
    return true;
}

static void eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
                     const uint8_t *payload, uint16_t plen) {
    uint8_t frame[1518];
    eth_hdr_t *eh = (eth_hdr_t *)frame;
    memcpy(eh->dst, dst_mac, 6);
    memcpy(eh->src, s_mac, 6);
    eh->type = net_htons(ethertype);
    memcpy(frame + sizeof(eth_hdr_t), payload, plen);
    ne2000_send(frame, sizeof(eth_hdr_t) + plen);
}

static uint16_t s_ip_id = 1;

static void ip_send(const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                    uint8_t proto, const uint8_t *payload, uint16_t plen) {
    uint8_t buf[1480];
    ip_hdr_t *iph = (ip_hdr_t *)buf;
    iph->ihl_ver = 0x45;
    iph->tos = 0;
    iph->total_len = net_htons(sizeof(ip_hdr_t) + plen);
    iph->id = net_htons(s_ip_id++);
    iph->frag_off = 0;
    iph->ttl = 64;
    iph->proto = proto;
    iph->checksum = 0;
    memcpy(iph->src, s_ip, 4);
    memcpy(iph->dst, dst_ip, 4);
    iph->checksum = net_htons(net_checksum(iph, sizeof(ip_hdr_t)));
    memcpy(buf + sizeof(ip_hdr_t), payload, plen);
    eth_send(dst_mac, ETHERTYPE_IP, buf, sizeof(ip_hdr_t) + plen);
}

static bool resolve_mac(const uint8_t ip[4], uint8_t mac_out[6]) {
    if (arp_lookup(ip, mac_out))
        return true;
    const uint8_t *target = ip_is_local(ip) ? ip : s_gw;
    arp_request(target);
    uint32_t t = time_millis() + 500;
    while (time_millis() < t) {
        net_poll();
        task_yield();
        if (arp_lookup(target, mac_out))
            return true;
    }
    return false;
}

static arp_entry_t s_arp_cache[ARP_CACHE_SIZE];

void arp_request(const uint8_t ip[4]) {
    arp_pkt_t pkt;
    pkt.htype_hi = 0;
    pkt.htype_lo = 1;
    pkt.ptype_hi = 0x08;
    pkt.ptype_lo = 0x00;
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.op = net_htons(ARP_OP_REQUEST);
    memcpy(pkt.sha, s_mac, 6);
    memcpy(pkt.spa, s_ip, 4);
    memset(pkt.tha, 0, 6);
    memcpy(pkt.tpa, ip, 4);
    eth_send(MAC_BCAST, ETHERTYPE_ARP, (uint8_t *)&pkt, sizeof(pkt));
}

void arp_announce(void) { arp_request(s_ip); }

static void arp_insert(const uint8_t ip[4], const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!s_arp_cache[i].valid || memcmp(s_arp_cache[i].ip, ip, 4) == 0) {
            memcpy(s_arp_cache[i].ip, ip, 4);
            memcpy(s_arp_cache[i].mac, mac, 6);
            s_arp_cache[i].valid = true;
            return;
        }
    }
    memcpy(s_arp_cache[0].ip, ip, 4);
    memcpy(s_arp_cache[0].mac, mac, 6);
    s_arp_cache[0].valid = true;
}

void arp_cache_insert(const uint8_t ip[4], const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_arp_cache[i].valid && memcmp(s_arp_cache[i].ip, ip, 4) == 0) {
            memcpy(s_arp_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!s_arp_cache[i].valid) {
            memcpy(s_arp_cache[i].ip, ip, 4);
            memcpy(s_arp_cache[i].mac, mac, 6);
            s_arp_cache[i].valid = true;
            return;
        }
    }
    memcpy(s_arp_cache[0].ip, ip, 4);
    memcpy(s_arp_cache[0].mac, mac, 6);
}

bool arp_lookup(const uint8_t ip[4], uint8_t mac_out[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_arp_cache[i].valid && memcmp(s_arp_cache[i].ip, ip, 4) == 0) {
            memcpy(mac_out, s_arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_reply(const uint8_t dst_ip[4], const uint8_t dst_mac[6]) {
    arp_pkt_t pkt;
    pkt.htype_hi = 0;
    pkt.htype_lo = 1;
    pkt.ptype_hi = 0x08;
    pkt.ptype_lo = 0x00;
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.op = net_htons(ARP_OP_REPLY);
    memcpy(pkt.sha, s_mac, 6);
    memcpy(pkt.spa, s_ip, 4);
    memcpy(pkt.tha, dst_mac, 6);
    memcpy(pkt.tpa, dst_ip, 4);
    eth_send(dst_mac, ETHERTYPE_ARP, (uint8_t *)&pkt, sizeof(pkt));
}

static void handle_arp(const uint8_t *data, uint16_t len) {
    if (len < sizeof(arp_pkt_t))
        return;
    const arp_pkt_t *arp = (const arp_pkt_t *)data;
    uint16_t op = net_ntohs(arp->op);

    if (op == ARP_OP_REPLY) {
        arp_cache_insert(arp->spa, arp->sha);
    } else if (op == ARP_OP_REQUEST) {
        uint8_t my_ip[4];
        net_get_ip(my_ip);
        if (memcmp(arp->tpa, my_ip, 4) == 0) {
            arp_cache_insert(arp->spa, arp->sha);
            arp_reply(arp->spa, arp->sha);
        }
    }
}

static void handle_icmp(const ip_hdr_t *iph, const uint8_t *data,
                        uint16_t len) {
    if (len < sizeof(icmp_hdr_t))
        return;
    const icmp_hdr_t *ih = (const icmp_hdr_t *)data;

    if (ih->type == ICMP_ECHO_REQ) {
        uint8_t buf[512];
        icmp_hdr_t *rep = (icmp_hdr_t *)buf;
        uint16_t payload_len = len - sizeof(icmp_hdr_t);
        if (payload_len > 512 - (uint16_t)sizeof(icmp_hdr_t))
            payload_len = 512 - (uint16_t)sizeof(icmp_hdr_t);
        rep->type = ICMP_ECHO_REP;
        rep->code = 0;
        rep->checksum = 0;
        rep->id = ih->id;
        rep->seq = ih->seq;
        memcpy(buf + sizeof(icmp_hdr_t), data + sizeof(icmp_hdr_t),
               payload_len);
        uint16_t total = sizeof(icmp_hdr_t) + payload_len;
        rep->checksum = net_htons(net_checksum(buf, total));
        uint8_t mac[6];
        if (arp_lookup(iph->src, mac))
            ip_send(mac, iph->src, IP_PROTO_ICMP, buf, total);
    } else if (ih->type == ICMP_ECHO_REP) {
        if (net_ntohs(ih->id) == s_ping_id && net_ntohs(ih->seq) == s_ping_seq)
            s_ping_reply = true;
    }
}

static void handle_udp(const ip_hdr_t *iph, const uint8_t *data, uint16_t len) {
    if (len < sizeof(udp_hdr_t))
        return;
    const udp_hdr_t *uh = (const udp_hdr_t *)data;
    uint16_t dport = net_ntohs(uh->dst_port);
    uint16_t sport = net_ntohs(uh->src_port);
    const uint8_t *payload = data + sizeof(udp_hdr_t);
    uint16_t plen = net_ntohs(uh->length) - sizeof(udp_hdr_t);
    if (plen > len - sizeof(udp_hdr_t))
        return;

    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!s_socks[i].used)
            continue;
        if (s_socks[i].proto != IP_PROTO_UDP)
            continue;
        if (s_socks[i].local_port != dport)
            continue;
        if (s_socks[i].rx_ready)
            continue;
        uint16_t copy = plen < NET_UDP_BUFSIZE ? plen : NET_UDP_BUFSIZE;
        memcpy(s_socks[i].rx_buf, payload, copy);
        s_socks[i].rx_len = copy;
        memcpy(s_socks[i].remote_ip, iph->src, 4);
        s_socks[i].remote_port = sport;
        arp_lookup(iph->src, s_socks[i].remote_mac);
        s_socks[i].rx_ready = true;
        break;
    }
}

static void tcp_send_flags(net_socket_t *s, uint8_t flags, const uint8_t *data,
                           uint16_t dlen);

static void handle_tcp(const ip_hdr_t *iph, const uint8_t *data, uint16_t len) {
    if (len < sizeof(tcp_hdr_t))
        return;
    const tcp_hdr_t *th = (const tcp_hdr_t *)data;
    uint16_t dport = net_ntohs(th->dst_port);
    uint16_t sport = net_ntohs(th->src_port);
    uint8_t hlen = (th->data_off >> 4) * 4;
    const uint8_t *payload = data + hlen;
    uint16_t plen = len > hlen ? len - hlen : 0;
    uint32_t seg_seq = net_ntohl(th->seq);
    uint32_t seg_ack = net_ntohl(th->ack);

    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = &s_socks[i];
        if (!s->used || s->proto != IP_PROTO_TCP)
            continue;
        if (s->local_port != dport)
            continue;
        if (memcmp(s->remote_ip, iph->src, 4) != 0)
            continue;
        if (s->remote_port != 0 && s->remote_port != sport)
            continue;

        if (th->flags & TCP_RST) {
            if (s->tcp_state == TCP_SYN_SENT) {
                if ((th->flags & TCP_ACK) && seg_ack == s->tx_seq)
                    s->tcp_state = TCP_CLOSED;
            } else {
                s->tcp_state = TCP_CLOSED;
            }
            return;
        }

        switch (s->tcp_state) {
        case TCP_SYN_SENT:
            if ((th->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                if (seg_ack != s->tx_seq)
                    break;
                s->rx_seq = seg_seq + 1;
                s->tx_seq = seg_ack;
                s->tcp_state = TCP_ESTABLISHED;
                tcp_send_flags(s, TCP_ACK, NULL, 0);
            }
            break;
        case TCP_ESTABLISHED:
            if (plen > 0) {
                uint16_t free =
                    NET_TCP_BUFSIZE -
                    ((s->tcp_rx_tail - s->tcp_rx_head + NET_TCP_BUFSIZE) %
                     NET_TCP_BUFSIZE);
                uint16_t copy = plen < free ? plen : free;
                for (uint16_t j = 0; j < copy; j++) {
                    s->tcp_rx[s->tcp_rx_tail] = payload[j];
                    s->tcp_rx_tail = (s->tcp_rx_tail + 1) % NET_TCP_BUFSIZE;
                }
                s->rx_seq = seg_seq + plen;
                tcp_send_flags(s, TCP_ACK, NULL, 0);
            }
            if (th->flags & TCP_FIN) {
                s->rx_seq = seg_seq + 1;
                s->tcp_state = TCP_CLOSE_WAIT;
                tcp_send_flags(s, TCP_ACK, NULL, 0);
            }
            if ((th->flags & TCP_ACK) && seg_ack > s->tx_seq)
                s->tx_seq = seg_ack;
            break;
        case TCP_FIN_WAIT1:
            if (th->flags & TCP_ACK)
                s->tcp_state = TCP_FIN_WAIT2;
            if (th->flags & TCP_FIN) {
                s->rx_seq = seg_seq + 1;
                tcp_send_flags(s, TCP_ACK, NULL, 0);
                s->tcp_state = TCP_TIME_WAIT;
            }
            break;
        case TCP_FIN_WAIT2:
            if (th->flags & TCP_FIN) {
                s->rx_seq = seg_seq + 1;
                tcp_send_flags(s, TCP_ACK, NULL, 0);
                s->tcp_state = TCP_TIME_WAIT;
            }
            break;
        case TCP_LAST_ACK:
            if (th->flags & TCP_ACK)
                s->tcp_state = TCP_CLOSED;
            break;
        default:
            break;
        }
        return;
    }
}

static void handle_ip(const uint8_t *data, uint16_t len) {
    if (len < sizeof(ip_hdr_t))
        return;
    const ip_hdr_t *iph = (const ip_hdr_t *)data;
    if ((iph->ihl_ver >> 4) != 4)
        return;
    uint8_t ihl = (iph->ihl_ver & 0x0F) * 4;
    if (ihl < 20 || ihl > len)
        return;
    if (memcmp(iph->dst, s_ip, 4) != 0 && memcmp(iph->dst, IP_BCAST, 4) != 0)
        return;

    const uint8_t *payload = data + ihl;
    uint16_t plen = net_ntohs(iph->total_len) - ihl;
    if (plen > len - ihl)
        plen = len - ihl;

    switch (iph->proto) {
    case IP_PROTO_ICMP:
        handle_icmp(iph, payload, plen);
        break;
    case IP_PROTO_UDP:
        handle_udp(iph, payload, plen);
        break;
    case IP_PROTO_TCP:
        handle_tcp(iph, payload, plen);
        break;
    }
}

void net_rx_enqueue(const uint8_t *frame, uint16_t len) {
    uint8_t next = (s_rx_head + 1) % RX_QUEUE_SIZE;
    if (next == s_rx_tail)
        return;
    if (len > RX_PKT_MAX)
        len = RX_PKT_MAX;
    memcpy(s_rx_queue[s_rx_head].data, frame, len);
    s_rx_queue[s_rx_head].len = len;
    s_rx_head = next;
}

static void net_rx_process(const uint8_t *frame, uint16_t len) {
    if (len < sizeof(eth_hdr_t))
        return;
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    uint16_t type = net_ntohs(eth->type);
    const uint8_t *payload = frame + sizeof(eth_hdr_t);
    uint16_t plen = len - sizeof(eth_hdr_t);
    if (type == ETHERTYPE_ARP)
        handle_arp(payload, plen);
    else if (type == ETHERTYPE_IP)
        handle_ip(payload, plen);
}

void net_init(void) {
    s_socks = (net_socket_t *)kzalloc(NET_MAX_SOCKETS * sizeof(net_socket_t));
    s_rx_queue = (rx_pkt_t *)kzalloc(RX_QUEUE_SIZE * sizeof(rx_pkt_t));
    memset(s_arp_cache, 0, sizeof(s_arp_cache));
    ne2000_get_mac(s_mac);
    ne2000_set_rx_callback(net_rx_enqueue);
}

void net_set_ip(const uint8_t ip[4], const uint8_t gw[4], const uint8_t nm[4]) {
    memcpy(s_ip, ip, 4);
    memcpy(s_gw, gw, 4);
    memcpy(s_nm, nm, 4);
}

void net_get_ip(uint8_t ip[4]) { memcpy(ip, s_ip, 4); }

bool icmp_ping(const uint8_t dst_ip[4], uint16_t id, uint16_t seq,
               uint32_t timeout_ms) {
    uint8_t mac[6];
    if (!resolve_mac(dst_ip, mac))
        return false;

    s_ping_id = id;
    s_ping_seq = seq;
    s_ping_reply = false;

    uint8_t buf[sizeof(icmp_hdr_t) + 32];
    icmp_hdr_t *ih = (icmp_hdr_t *)buf;
    ih->type = ICMP_ECHO_REQ;
    ih->code = 0;
    ih->checksum = 0;
    ih->id = net_htons(id);
    ih->seq = net_htons(seq);
    memset(buf + sizeof(icmp_hdr_t), 0xAB, 32);
    ih->checksum = net_htons(net_checksum(buf, sizeof(buf)));
    ip_send(mac, dst_ip, IP_PROTO_ICMP, buf, sizeof(buf));

    uint32_t deadline = time_millis() + timeout_ms;
    while (time_millis() < deadline) {
        task_yield();
        net_poll();
        if (s_ping_reply)
            return true;
    }
    return false;
}

int udp_open(uint16_t local_port) {
    if (!s_socks)
        return -1;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!s_socks[i].used) {
            memset(&s_socks[i], 0, sizeof(net_socket_t));
            s_socks[i].rx_buf = (uint8_t *)kmalloc(NET_UDP_BUFSIZE);
            if (!s_socks[i].rx_buf)
                return -1;
            s_socks[i].used = true;
            s_socks[i].proto = IP_PROTO_UDP;
            s_socks[i].local_port = local_port;
            return i;
        }
    }
    return -1;
}

void udp_close(int sock) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !s_socks)
        return;
    kfree(s_socks[sock].rx_buf);
    s_socks[sock].rx_buf = NULL;
    s_socks[sock].used = false;
}

bool udp_send(int sock, const uint8_t dst_ip[4], uint16_t dst_port,
              const uint8_t *data, uint16_t len) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS)
        return false;
    net_socket_t *s = &s_socks[sock];
    if (!s->used)
        return false;
    uint8_t mac[6];
    if (!resolve_mac(dst_ip, mac))
        return false;
    uint16_t total = sizeof(udp_hdr_t) + len;
    uint8_t buf[1460];
    if (total > 1460)
        return false;
    udp_hdr_t *uh = (udp_hdr_t *)buf;
    uh->src_port = net_htons(s->local_port);
    uh->dst_port = net_htons(dst_port);
    uh->length = net_htons(total);
    uh->checksum = 0;
    memcpy(buf + sizeof(udp_hdr_t), data, len);
    uh->checksum =
        net_htons(udp_tcp_checksum(s_ip, dst_ip, IP_PROTO_UDP, buf, total));
    ip_send(mac, dst_ip, IP_PROTO_UDP, buf, total);
    return true;
}

int udp_recv(int sock, uint8_t *buf, uint16_t maxlen, uint8_t src_ip[4],
             uint16_t *src_port) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS)
        return -1;
    net_socket_t *s = &s_socks[sock];
    if (!s->used || !s->rx_ready)
        return 0;
    uint16_t copy = s->rx_len < maxlen ? s->rx_len : maxlen;
    memcpy(buf, s->rx_buf, copy);
    if (src_ip)
        memcpy(src_ip, s->remote_ip, 4);
    if (src_port)
        *src_port = s->remote_port;
    s->rx_ready = false;
    return (int)copy;
}

static void tcp_send_flags(net_socket_t *s, uint8_t flags, const uint8_t *data,
                           uint16_t dlen) {
    uint8_t mac[6];
    if (!arp_lookup(s->remote_ip, mac)) {
        if (!resolve_mac(s->remote_ip, mac))
            return;
    }
    uint16_t total = sizeof(tcp_hdr_t) + dlen;
    uint8_t buf[1460];
    if (total > 1460)
        return;
    tcp_hdr_t *th = (tcp_hdr_t *)buf;
    th->src_port = net_htons(s->local_port);
    th->dst_port = net_htons(s->remote_port);
    th->seq = net_htonl(s->tx_seq);
    th->ack = net_htonl(s->rx_seq);
    th->data_off = (sizeof(tcp_hdr_t) / 4) << 4;
    th->flags = flags;
    th->window = net_htons(NET_TCP_BUFSIZE);
    th->checksum = 0;
    th->urgent = 0;
    if (dlen)
        memcpy(buf + sizeof(tcp_hdr_t), data, dlen);
    th->checksum = net_htons(
        udp_tcp_checksum(s_ip, s->remote_ip, IP_PROTO_TCP, buf, total));
    ip_send(mac, s->remote_ip, IP_PROTO_TCP, buf, total);
    if (flags & (TCP_SYN | TCP_FIN))
        s->tx_seq++;
    if (dlen)
        s->tx_seq += dlen;
}

int tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                uint16_t local_port) {
    if (!s_socks)
        return -1;
    int i;
    for (i = 0; i < NET_MAX_SOCKETS; i++)
        if (!s_socks[i].used)
            break;
    if (i == NET_MAX_SOCKETS)
        return -1;

    static uint16_t s_next_port = 49200;
    uint16_t actual_port = s_next_port++;
    if (s_next_port >= 49900)
        s_next_port = 49200;

    net_socket_t *s = &s_socks[i];
    memset(s, 0, sizeof(net_socket_t));
    s->rx_buf = (uint8_t *)kmalloc(NET_UDP_BUFSIZE);
    s->tcp_rx = (uint8_t *)kmalloc(NET_TCP_BUFSIZE);
    if (!s->rx_buf || !s->tcp_rx) {
        kfree(s->rx_buf);
        kfree(s->tcp_rx);
        return -1;
    }
    s->used = true;
    s->proto = IP_PROTO_TCP;
    s->local_port = actual_port;
    s->remote_port = dst_port;
    memcpy(s->remote_ip, dst_ip, 4);
    s->tx_seq = 0x12345678;
    s->tcp_state = TCP_SYN_SENT;

    uint8_t mac[6];
    if (!resolve_mac(dst_ip, mac)) {
        kfree(s->rx_buf);
        kfree(s->tcp_rx);
        s->used = false;
        return -1;
    }
    memcpy(s->remote_mac, mac, 6);
    tcp_send_flags(s, TCP_SYN, NULL, 0);

    uint32_t deadline = time_millis() + 3000;
    while (time_millis() < deadline) {
        task_yield();
        if (s->tcp_state == TCP_ESTABLISHED)
            return i;
        if (s->tcp_state == TCP_CLOSED)
            break;
    }
    if (s->tcp_state != TCP_CLOSED)
        tcp_send_flags(s, TCP_RST | TCP_ACK, NULL, 0);
    kfree(s->rx_buf);
    kfree(s->tcp_rx);
    s->used = false;
    return -1;
}

void tcp_close(int sock) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !s_socks)
        return;
    net_socket_t *s = &s_socks[sock];
    if (!s->used)
        return;
    if (s->tcp_state == TCP_ESTABLISHED || s->tcp_state == TCP_CLOSE_WAIT) {
        tcp_state_t next =
            (s->tcp_state == TCP_CLOSE_WAIT) ? TCP_LAST_ACK : TCP_FIN_WAIT1;
        tcp_send_flags(s, TCP_FIN | TCP_ACK, NULL, 0);
        s->tcp_state = next;
        uint32_t deadline = time_millis() + 2000;
        while (time_millis() < deadline) {
            task_yield();
            if (s->tcp_state == TCP_CLOSED || s->tcp_state == TCP_TIME_WAIT)
                break;
        }
    }
    kfree(s->rx_buf);
    s->rx_buf = NULL;
    kfree(s->tcp_rx);
    s->tcp_rx = NULL;
    s->used = false;
}

bool tcp_send(int sock, const uint8_t *data, uint16_t len) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS)
        return false;
    net_socket_t *s = &s_socks[sock];
    if (!s->used || s->tcp_state != TCP_ESTABLISHED)
        return false;
    uint16_t mss = 1460 - sizeof(tcp_hdr_t);
    while (len > 0) {
        uint16_t chunk = len < mss ? len : mss;
        tcp_send_flags(s, TCP_PSH | TCP_ACK, data, chunk);
        data += chunk;
        len -= chunk;
        if (s->tcp_state != TCP_ESTABLISHED)
            return false;
    }
    return true;
}

int tcp_recv(int sock, uint8_t *buf, uint16_t maxlen) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS)
        return -1;
    net_socket_t *s = &s_socks[sock];
    if (!s->used)
        return -1;
    uint16_t count = 0;
    while (count < maxlen && s->tcp_rx_head != s->tcp_rx_tail) {
        buf[count++] = s->tcp_rx[s->tcp_rx_head];
        s->tcp_rx_head = (s->tcp_rx_head + 1) % NET_TCP_BUFSIZE;
    }
    return (int)count;
}

tcp_state_t tcp_state(int sock) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS)
        return TCP_CLOSED;
    return s_socks[sock].tcp_state;
}

static bool s_polling = false;

void net_poll(void) {
    if (s_polling)
        return;
    s_polling = true;
    ne2000_poll();
    while (s_rx_tail != s_rx_head) {
        rx_pkt_t *p = &s_rx_queue[s_rx_tail];
        net_rx_process(p->data, p->len);
        s_rx_tail = (s_rx_tail + 1) % RX_QUEUE_SIZE;
    }
    s_polling = false;
}
