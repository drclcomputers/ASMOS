// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "network/dns.h"
#include "drivers/ne2000.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "network/net.h"
#include "os/scheduler.h"

#define DNS_PKT_SIZE 512
#define DNS_LOCAL_BASE 49160
#define DNS_LOCAL_COUNT 8

static uint8_t s_dns_server[4] = {8, 8, 8, 8};

#define DNS_CACHE_N 8
typedef struct {
    bool used;
    char name[64];
    uint8_t ip[4];
} dns_cache_t;
static dns_cache_t s_cache[DNS_CACHE_N];
static unsigned s_cache_next;
static uint16_t s_dns_txn_id = 0x1234;
static uint16_t s_dns_local_port = DNS_LOCAL_BASE;

void dns_set_server(const uint8_t ip[4]) {
    if (ip)
        memcpy(s_dns_server, ip, 4);
}

void dns_get_server(uint8_t ip[4]) { memcpy(ip, s_dns_server, 4); }

static uint16_t encode_name(uint8_t *buf, uint16_t max_len, const char *name) {
    uint16_t pos = 0;
    while (*name && pos < max_len) {
        const char *dot = name;
        while (*dot && *dot != '.')
            dot++;
        uint8_t len = (uint8_t)(dot - name);
        if (len == 0 || len > 63 || pos + len + 1 >= max_len)
            return 0;
        buf[pos++] = len;
        for (uint8_t i = 0; i < len; i++)
            buf[pos++] = (uint8_t)name[i];
        name = dot;
        if (*name == '.')
            name++;
    }
    if (pos >= max_len)
        return 0;
    buf[pos++] = 0;
    return pos;
}

/* Returns the offset just past the name at `pos` (in the record being
 * parsed). A compression pointer ends the name in place: 2 bytes, no jump.
 * (Following the pointer and returning ITS end made every answer record
 * parse from the wrong offset, which is why lookups always failed.) */
static uint16_t skip_name(const uint8_t *pkt, uint16_t pkt_len, uint16_t pos) {
    while (pos < pkt_len) {
        uint8_t len = pkt[pos];
        if (len == 0)
            return pos + 1;
        if ((len & 0xC0) == 0xC0)
            return (pos + 1 < pkt_len) ? pos + 2 : 0;
        if (len > 63)
            return 0;
        pos += len + 1;
    }
    return 0;
}

/* a dotted IPv4 address ("10.0.2.2"): its 4 bytes */
static bool ip_literal(const char *h, uint8_t ip[4]) {
    int part = 0, digits = 0;
    uint32_t val = 0;
    for (int i = 0;; i++) {
        char c = h[i];
        if (c >= '0' && c <= '9') {
            val = val * 10 + (uint32_t)(c - '0');
            if (++digits > 3 || val > 255)
                return false;
        } else if ((c == '.' || !c) && digits && part < 4) {
            ip[part++] = (uint8_t)val;
            val = 0;
            digits = 0;
            if (!c)
                return part == 4;
        } else {
            return false;
        }
    }
}

static bool cache_find(const char *name, uint8_t ip[4]) {
    for (int c = 0; c < DNS_CACHE_N; c++)
        if (s_cache[c].used && strcmp(s_cache[c].name, name) == 0) {
            memcpy(ip, s_cache[c].ip, 4);
            return true;
        }
    return false;
}

static void cache_keep(const char *name, const uint8_t ip[4]) {
    if (strlen(name) >= sizeof(s_cache[0].name))
        return;
    dns_cache_t *e = &s_cache[s_cache_next++ % DNS_CACHE_N];
    e->used = true;
    strcpy(e->name, name);
    memcpy(e->ip, ip, 4);
}

/* the question for name, id: its length in pkt, 0 if the name is bad */
static uint16_t build_query(uint8_t *pkt, uint16_t id, const char *name) {
    memset(pkt, 0, 12);
    pkt[0] = (uint8_t)(id >> 8);
    pkt[1] = (uint8_t)id;
    pkt[2] = 0x01; /* RD: recursion, please */
    pkt[5] = 0x01; /* QDCOUNT=1 */
    uint16_t n = encode_name(pkt + 12, DNS_PKT_SIZE - 12 - 4, name);
    if (!n)
        return 0;
    uint16_t pos = 12 + n;
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01; /* QTYPE  A  */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01; /* QCLASS IN */
    return pos;
}

/* the answer to question id: its first A record. 1 found, 0 not this
 * answer (or none in it) */
static int parse_answer(const uint8_t *resp, int rlen, uint16_t id,
                        uint8_t ip_out[4]) {
    if (rlen <= 12 || ((resp[0] << 8) | resp[1]) != id || !(resp[2] & 0x80) ||
        (resp[3] & 0x0F) != 0)
        return 0;
    uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
    uint16_t pos = skip_name(resp, (uint16_t)rlen, 12);
    if (pos == 0 || pos + 4 > rlen)
        return 0;
    pos += 4;
    for (uint16_t a = 0; a < ancount; a++) {
        pos = skip_name(resp, (uint16_t)rlen, pos);
        if (pos == 0 || pos + 10 > rlen)
            return 0;
        uint16_t type = (uint16_t)((resp[pos] << 8) | resp[pos + 1]);
        uint16_t rdlen = (uint16_t)((resp[pos + 8] << 8) | resp[pos + 9]);
        pos += 10;
        if (pos + rdlen > rlen)
            return 0;
        if (type == 1 && rdlen == 4) {
            memcpy(ip_out, resp + pos, 4);
            return 1;
        }
        pos += rdlen;
    }
    return 0;
}

// lookups that don't wait: dns_start, then dns_poll every frame

#define DNS_Q_N 4
typedef struct {
    bool used;
    int8_t result; /* 1 found, -1 failed, 0 asking */
    char name[64];
    uint8_t ip[4];
    uint16_t id;
    int sock, attempt;
    uint32_t deadline;
} dns_q_t;
static dns_q_t s_q[DNS_Q_N];

/* (the question sent again: the next attempt) */
static void q_send(dns_q_t *q) {
    if (q->sock >= 0)
        udp_close(q->sock);
    q->sock = -1;
    while (q->attempt < DNS_RETRIES) {
        q->attempt++;
        q->id = s_dns_txn_id++;
        q->deadline = time_millis() + DNS_TIMEOUT_MS;
        uint8_t *pkt = (uint8_t *)kmalloc(DNS_PKT_SIZE);
        uint16_t len = pkt ? build_query(pkt, q->id, q->name) : 0;
        uint16_t local =
            DNS_LOCAL_BASE + (s_dns_local_port++ % DNS_LOCAL_COUNT);
        int sock = len ? udp_open(local) : -1;
        bool sent =
            sock >= 0 && udp_send(sock, s_dns_server, DNS_PORT, pkt, len);
        kfree(pkt);
        if (sent) {
            q->sock = sock;
            return;
        }
        if (sock >= 0)
            udp_close(sock);
        if (!len)
            break; /* (a bad name: no use trying again) */
    }
    q->result = -1;
}

int dns_start(const char *hostname) {
    if (!hostname || !hostname[0] || strlen(hostname) >= 64)
        return -1;
    for (int i = 0; i < DNS_Q_N; i++) {
        dns_q_t *q = &s_q[i];
        if (q->used)
            continue;
        memset(q, 0, sizeof(*q));
        q->used = true;
        q->sock = -1;
        strcpy(q->name, hostname);
        if (ip_literal(hostname, q->ip) || cache_find(hostname, q->ip))
            q->result = 1;
        else
            q_send(q);
        return i;
    }
    return -1;
}

int dns_poll(int h, uint8_t ip_out[4]) {
    if (h < 0 || h >= DNS_Q_N || !s_q[h].used)
        return -1;
    dns_q_t *q = &s_q[h];
    if (q->result == 0 && q->sock >= 0) {
        uint8_t *resp = (uint8_t *)kmalloc(DNS_PKT_SIZE);
        int rlen = resp ? udp_recv(q->sock, resp, DNS_PKT_SIZE, NULL, NULL) : 0;
        if (rlen > 0 && parse_answer(resp, rlen, q->id, q->ip)) {
            q->result = 1;
            cache_keep(q->name, q->ip);
        }
        kfree(resp);
        if (!q->result && (int32_t)(time_millis() - q->deadline) >= 0)
            q_send(q); /* (no answer in time: again, or failed) */
    }
    int r = q->result;
    if (r == 1)
        memcpy(ip_out, q->ip, 4);
    if (r != 0)
        dns_cancel(h);
    return r;
}

void dns_cancel(int h) {
    if (h < 0 || h >= DNS_Q_N || !s_q[h].used)
        return;
    if (s_q[h].sock >= 0)
        udp_close(s_q[h].sock);
    s_q[h].used = false;
}

/* waits (gopher, ftp, the shell): the same, polled here */
bool dns_resolve(const char *hostname, uint8_t ip_out[4]) {
    int h = dns_start(hostname);
    if (h < 0)
        return false;
    int r;
    while ((r = dns_poll(h, ip_out)) == 0) {
        net_poll();
        task_yield();
    }
    return r == 1;
}
