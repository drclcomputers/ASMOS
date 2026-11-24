// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "network/dhcp.h"
#include "drivers/ne2000.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "network/dns.h"
#include "network/net.h"

#define BOOTREQUEST 1
#define BOOTREPLY 2
#define MAGIC 0x63825363u

enum { DISCOVER = 1, OFFER, REQUEST, DECLINE, ACK, NAK, RELEASE };

/* options */
#define OPT_PAD 0
#define OPT_MASK 1
#define OPT_ROUTER 3
#define OPT_DNS 6
#define OPT_HOSTNAME 12
#define OPT_REQ_IP 50
#define OPT_LEASE 51
#define OPT_TYPE 53
#define OPT_SERVER 54
#define OPT_PARAMS 55
#define OPT_T1 58
#define OPT_T2 59
#define OPT_CLIENT_ID 61
#define OPT_END 255

typedef struct __attribute__((packed)) {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint8_t ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[312];
} dhcp_pkt_t;

static dhcp_state_t s_state = DHCP_OFF;
static uint32_t s_xid;
static uint8_t s_mac[6];
static uint8_t s_offer[4];      /* the address offered / leased */
static uint8_t s_server[4];     /* the server that gave it */
static uint8_t s_server_mac[6]; /* (for renewing without ARP) */
static uint8_t s_mask[4], s_gw[4], s_dns[4];
static uint32_t s_lease, s_t1, s_t2; /* seconds */
static uint32_t s_bound_at;          /* ms when the lease began */
static uint32_t s_next;              /* ms: the next try or step */
static int s_tries;

static const uint8_t ZERO[4] = {0, 0, 0, 0};
static const uint8_t BCAST_IP[4] = {255, 255, 255, 255};
static const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

const char *dhcp_state_name(dhcp_state_t s) {
    static const char *const n[] = {"off",   "searching", "requesting",
                                    "bound", "renewing",  "rebinding"};
    return (unsigned)s < 6 ? n[s] : "?";
}

dhcp_state_t dhcp_state(void) { return s_state; }

static uint32_t elapsed_s(void) { return (time_millis() - s_bound_at) / 1000; }

void dhcp_info(dhcp_info_t *o) {
    memset(o, 0, sizeof(*o));
    o->state = s_state;
    o->tries = s_tries;
    if (s_state < DHCP_BOUND)
        return;
    memcpy(o->ip, s_offer, 4);
    memcpy(o->netmask, s_mask, 4);
    memcpy(o->gateway, s_gw, 4);
    memcpy(o->dns, s_dns, 4);
    memcpy(o->server, s_server, 4);
    o->lease_s = s_lease;
    uint32_t e = elapsed_s();
    o->lease_left_s = s_lease == 0xFFFFFFFFu ? s_lease
                      : e < s_lease          ? s_lease - e
                                             : 0;
}

// sending

static uint8_t *opt(uint8_t *p, uint8_t code, const void *data, uint8_t len) {
    *p++ = code;
    *p++ = len;
    memcpy(p, data, len);
    return p + len;
}

static void send_msg(int type, bool broadcast) {
    static dhcp_pkt_t m;
    memset(&m, 0, sizeof(m));
    m.op = BOOTREQUEST;
    m.htype = 1; /* Ethernet */
    m.hlen = 6;
    m.xid = s_xid;
    /* ask for the answers broadcast: we can't take unicast yet */
    m.flags = net_htons(
        s_state == DHCP_SEARCHING || s_state == DHCP_REQUESTING ? 0x8000 : 0);
    memcpy(m.chaddr, s_mac, 6);
    m.magic = net_htonl(MAGIC);
    if (s_state == DHCP_RENEWING || s_state == DHCP_REBINDING ||
        type == RELEASE)
        memcpy(m.ciaddr, s_offer, 4);

    uint8_t *p = m.options, t = (uint8_t)type;
    p = opt(p, OPT_TYPE, &t, 1);
    uint8_t cid[7] = {1};
    memcpy(cid + 1, s_mac, 6);
    p = opt(p, OPT_CLIENT_ID, cid, 7);
    if (type == REQUEST && s_state == DHCP_REQUESTING) {
        p = opt(p, OPT_REQ_IP, s_offer, 4);
        p = opt(p, OPT_SERVER, s_server, 4);
    }
    if (type == RELEASE)
        p = opt(p, OPT_SERVER, s_server, 4);
    if (type != RELEASE) {
        static const uint8_t params[] = {OPT_MASK,  OPT_ROUTER, OPT_DNS,
                                         OPT_LEASE, OPT_T1,     OPT_T2};
        p = opt(p, OPT_PARAMS, params, sizeof(params));
        p = opt(p, OPT_HOSTNAME, "ASMOS", 5);
    }
    *p++ = OPT_END;
    uint16_t len = (uint16_t)(p - (uint8_t *)&m);
    if (len < 300) /* (some old servers want a full BOOTP packet) */
        len = 300;
    if (broadcast)
        net_send_udp(BCAST_MAC, BCAST_IP, 68, 67, (uint8_t *)&m, len);
    else
        net_send_udp(s_server_mac, s_server, 68, 67, (uint8_t *)&m, len);
}

static void new_xid(void) {
    s_xid = net_htonl(0x41534D00u ^ (time_millis() * 2654435761u) ^
                      ((uint32_t)s_mac[4] << 8 | s_mac[5]));
}

static void discover(void) {
    s_state = DHCP_SEARCHING;
    new_xid();
    send_msg(DISCOVER, true);
    /* 2, 4, 8, 16 s, then every 30 s */
    uint32_t wait = s_tries < 4 ? 2000u << s_tries : 30000u;
    s_tries++;
    s_next = time_millis() + wait;
}

void dhcp_start(void) {
    ne2000_get_mac(s_mac);
    s_tries = 0;
    net_set_ip(ZERO, ZERO, ZERO);
    discover();
}

void dhcp_stop(bool release) {
    if (release && s_state >= DHCP_BOUND)
        send_msg(RELEASE, false);
    s_state = DHCP_OFF;
}

// receiving

/* the value of option code, or NULL; *len its length */
static const uint8_t *find_opt(const uint8_t *o, int n, uint8_t code,
                               int *len) {
    int i = 0;
    while (i < n && o[i] != OPT_END) {
        if (o[i] == OPT_PAD) {
            i++;
            continue;
        }
        if (i + 1 >= n || i + 2 + o[i + 1] > n)
            return NULL;
        if (o[i] == code) {
            *len = o[i + 1];
            return o + i + 2;
        }
        i += 2 + o[i + 1];
    }
    return NULL;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           p[3];
}

static void take_lease(const uint8_t *o, int n) {
    int l;
    const uint8_t *v;
    memcpy(s_mask,
           (v = find_opt(o, n, OPT_MASK, &l)) && l >= 4
               ? v
               : (const uint8_t *)"\xFF\xFF\xFF\x00",
           4);
    memcpy(s_gw, (v = find_opt(o, n, OPT_ROUTER, &l)) && l >= 4 ? v : ZERO, 4);
    if ((v = find_opt(o, n, OPT_DNS, &l)) && l >= 4)
        memcpy(s_dns, v, 4);
    s_lease = (v = find_opt(o, n, OPT_LEASE, &l)) && l == 4 ? be32(v) : 3600;
    if (s_lease < 60)
        s_lease = 60;
    s_t1 = (v = find_opt(o, n, OPT_T1, &l)) && l == 4 ? be32(v) : s_lease / 2;
    s_t2 =
        (v = find_opt(o, n, OPT_T2, &l)) && l == 4 ? be32(v) : s_lease / 8 * 7;
    s_bound_at = time_millis();
    s_state = DHCP_BOUND;
    s_tries = 0;
    net_set_ip(s_offer, s_gw, s_mask);
    if (memcmp(s_dns, ZERO, 4))
        dns_set_server(s_dns);
    arp_announce(); /* tell the network the address is in use */
}

void dhcp_input(const uint8_t src_mac[6], const uint8_t *data, uint16_t len) {
    if (s_state == DHCP_OFF || len < 240)
        return;
    const dhcp_pkt_t *m = (const dhcp_pkt_t *)data;
    if (m->op != BOOTREPLY || m->xid != s_xid || net_ntohl(m->magic) != MAGIC ||
        memcmp(m->chaddr, s_mac, 6))
        return;
    const uint8_t *o = data + 240;
    int n = len - 240, l;
    const uint8_t *t = find_opt(o, n, OPT_TYPE, &l);
    const uint8_t *sid = find_opt(o, n, OPT_SERVER, &l);
    if (!t)
        return;
    switch (s_state) {
    case DHCP_SEARCHING:
        if (*t != OFFER || !sid)
            return;
        memcpy(s_offer, m->yiaddr, 4);
        memcpy(s_server, sid, 4);
        memcpy(s_server_mac, src_mac, 6);
        s_state = DHCP_REQUESTING;
        send_msg(REQUEST, true);
        s_next = time_millis() + 4000;
        s_tries = 0;
        break;
    case DHCP_REQUESTING:
    case DHCP_RENEWING:
    case DHCP_REBINDING:
        if (*t == ACK) {
            if (sid && s_state == DHCP_REBINDING) {
                memcpy(s_server, sid, 4); /* another server took it */
                memcpy(s_server_mac, src_mac, 6);
            }
            memcpy(s_offer, m->yiaddr, 4);
            take_lease(o, n);
        } else if (*t == NAK) {
            s_tries = 0;
            net_set_ip(ZERO, ZERO, ZERO);
            discover();
        }
        break;
    default:
        break;
    }
}

// the clock

void dhcp_tick(void) {
    if (s_state == DHCP_OFF)
        return;
    uint32_t now = time_millis();
    if (s_state == DHCP_BOUND || s_state == DHCP_RENEWING ||
        s_state == DHCP_REBINDING) {
        if (s_lease == 0xFFFFFFFFu)
            return;
        uint32_t e = elapsed_s();
        if (e >= s_lease) { /* gone: start again */
            s_tries = 0;
            net_set_ip(ZERO, ZERO, ZERO);
            discover();
            return;
        }
        dhcp_state_t want = e >= s_t2   ? DHCP_REBINDING
                            : e >= s_t1 ? DHCP_RENEWING
                                        : DHCP_BOUND;
        if (want != s_state) {
            s_state = want;
            new_xid();
            s_next = now; /* ask now */
        }
        if (s_state != DHCP_BOUND && (int32_t)(now - s_next) >= 0) {
            send_msg(REQUEST, s_state == DHCP_REBINDING);
            s_next = now + 10000;
        }
        return;
    }
    if ((int32_t)(now - s_next) < 0)
        return;
    if (s_state == DHCP_REQUESTING && s_tries++ < 3) {
        send_msg(REQUEST, true);
        s_next = now + 4000;
        return;
    }
    discover();
}
