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

bool dns_resolve(const char *hostname, uint8_t ip_out[4]) {
    if (!hostname)
        return false;

    bool all_digits = true;
    int dots = 0;
    for (int i = 0; hostname[i]; i++) {
        char c = hostname[i];
        if (c == '.') {
            dots++;
        } else if (c < '0' || c > '9') {
            all_digits = false;
            break;
        }
    }
    if (all_digits && dots == 3) {
        uint8_t parts[4] = {0};
        int part = 0;
        uint16_t val = 0;
        for (int i = 0; hostname[i]; i++) {
            char c = hostname[i];
            if (c == '.') {
                parts[part++] = (uint8_t)val;
                val = 0;
            } else {
                val = val * 10 + (c - '0');
            }
        }
        parts[3] = (uint8_t)val;
        memcpy(ip_out, parts, 4);
        return true;
    }

    /* small cache: gopher menus repeat the same host on every line */
    for (int c = 0; c < DNS_CACHE_N; c++)
        if (s_cache[c].used && strcmp(s_cache[c].name, hostname) == 0) {
            memcpy(ip_out, s_cache[c].ip, 4);
            return true;
        }

    uint8_t *pkt = (uint8_t *)kmalloc(DNS_PKT_SIZE);
    uint8_t *resp = (uint8_t *)kmalloc(DNS_PKT_SIZE);
    if (!pkt || !resp) {
        kfree(pkt);
        kfree(resp);
        return false;
    }

    bool found = false;
    uint16_t txn_id = s_dns_txn_id;

    for (int attempt = 0; attempt < DNS_RETRIES && !found; attempt++) {
        uint16_t sent_id = txn_id;
        memset(pkt, 0, DNS_PKT_SIZE);
        pkt[0] = (uint8_t)(txn_id >> 8);
        pkt[1] = (uint8_t)(txn_id & 0xFF);
        pkt[2] = 0x00;
        pkt[3] = 0x01; /* RD */
        pkt[4] = 0x00;
        pkt[5] = 0x01; /* QDCOUNT=1 */

        uint16_t pos = 12;
        uint16_t name_len =
            encode_name(pkt + pos, DNS_PKT_SIZE - pos - 4, hostname);
        if (name_len == 0) {
            txn_id++;
            continue;
        }
        pos += name_len;
        pkt[pos++] = 0x00;
        pkt[pos++] = 0x01; /* QTYPE  A  */
        pkt[pos++] = 0x00;
        pkt[pos++] = 0x01; /* QCLASS IN */

        uint16_t local =
            DNS_LOCAL_BASE + (s_dns_local_port++ % DNS_LOCAL_COUNT);
        int sock = udp_open(local);
        if (sock < 0) {
            txn_id++;
            continue;
        }

        if (!udp_send(sock, s_dns_server, DNS_PORT, pkt, pos)) {
            udp_close(sock);
            txn_id++;
            continue;
        }

        uint32_t deadline = time_millis() + DNS_TIMEOUT_MS;
        int rlen = 0;
        while (time_millis() < deadline) {
            net_poll();
            task_yield();
            rlen = udp_recv(sock, resp, DNS_PKT_SIZE, NULL, NULL);
            if (rlen > 12)
                break;
        }
        udp_close(sock);

        if (rlen <= 12) {
            txn_id++;
            continue;
        }

        uint16_t resp_id = (resp[0] << 8) | resp[1];
        if (resp_id != sent_id) {
            txn_id++;
            continue;
        }
        if (!(resp[2] & 0x80)) {
            txn_id++;
            continue;
        }
        if ((resp[3] & 0x0F) != 0) {
            txn_id++;
            continue;
        }

        uint16_t ancount = (resp[6] << 8) | resp[7];
        if (ancount == 0) {
            txn_id++;
            continue;
        }

        pos = 12;
        pos = skip_name(resp, (uint16_t)rlen, pos);
        if (pos == 0 || pos + 4 > rlen) {
            txn_id++;
            continue;
        }
        pos += 4;

        for (uint16_t a = 0; a < ancount; a++) {
            pos = skip_name(resp, (uint16_t)rlen, pos);
            if (pos == 0 || pos + 10 > rlen)
                break;
            uint16_t type = (resp[pos] << 8) | resp[pos + 1];
            uint16_t rdlen = (resp[pos + 8] << 8) | resp[pos + 9];
            pos += 10;
            if (pos + rdlen > rlen)
                break;
            if (type == 1 && rdlen == 4) {
                memcpy(ip_out, resp + pos, 4);
                found = true;
                break;
            }
            pos += rdlen;
        }
        txn_id++;
    }
    s_dns_txn_id = txn_id;

    kfree(pkt);
    kfree(resp);
    if (found && strlen(hostname) < sizeof(s_cache[0].name)) {
        dns_cache_t *e = &s_cache[s_cache_next++ % DNS_CACHE_N];
        e->used = true;
        strcpy(e->name, hostname);
        memcpy(e->ip, ip_out, 4);
    }
    return found;
}
