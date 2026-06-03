#include "network/dns.h"
#include "drivers/ne2000.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/time.h"
#include "network/net.h"

#define DNS_PKT_SIZE 512

static uint8_t s_dns_server[4] = {10, 0, 2, 3};
static uint16_t s_dns_txn_id = 0x1234;

void dns_set_server(const uint8_t ip[4]) {
    if (ip)
        memcpy(s_dns_server, ip, 4);
}

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

static uint16_t skip_name(const uint8_t *pkt, uint16_t pkt_len, uint16_t pos) {
    uint8_t jumps = 0;
    while (pos < pkt_len && jumps < 32) {
        uint8_t len = pkt[pos];
        if (len == 0)
            return pos + 1;
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= pkt_len)
                return 0;
            uint16_t offset = ((len & 0x3F) << 8) | pkt[pos + 1];
            if (offset >= pkt_len)
                return 0;
            pos = offset;
            jumps++;
            continue;
        }
        if (len > 63)
            return 0;
        pos += len + 1;
        if (pos >= pkt_len)
            return 0;
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

    uint8_t *pkt = (uint8_t *)kmalloc(DNS_PKT_SIZE);
    uint8_t *resp = (uint8_t *)kmalloc(DNS_PKT_SIZE);
    if (!pkt || !resp) {
        kfree(pkt);
        kfree(resp);
        return false;
    }

    bool found = false;

    for (int attempt = 0; attempt < DNS_RETRIES && !found; attempt++) {
            memset(pkt, 0, DNS_PKT_SIZE);
            pkt[0] = (uint8_t)(s_dns_txn_id >> 8);
            pkt[1] = (uint8_t)(s_dns_txn_id & 0xFF);
            pkt[2] = 0x00; /* Flags high byte */
            pkt[3] = 0x01; /* Flags low byte - RD bit set */
            pkt[4] = 0x00;
            pkt[5] = 0x01; /* QDCOUNT = 1 */

        uint16_t pos = 12;
        uint16_t name_len =
            encode_name(pkt + pos, DNS_PKT_SIZE - pos - 4, hostname);
        if (name_len == 0) {
            s_dns_txn_id++;
            continue;
        }
        pos += name_len;
        pkt[pos++] = 0x00;
        pkt[pos++] = 0x01; /* QTYPE A  */
        pkt[pos++] = 0x00;
        pkt[pos++] = 0x01; /* QCLASS IN */

        int sock = udp_open(DNS_LOCAL);
        if (sock < 0) {
            s_dns_txn_id++;
            continue;
        }

        if (!udp_send(sock, s_dns_server, DNS_PORT, pkt, pos)) {
            udp_close(sock);
            s_dns_txn_id++;
            continue;
        }

        uint32_t deadline = time_millis() + DNS_TIMEOUT_MS;
        int rlen = 0;
        while (time_millis() < deadline) {
            net_poll();
            rlen = udp_recv(sock, resp, DNS_PKT_SIZE, NULL, NULL);
            if (rlen > 12)
                break;
        }
        udp_close(sock);

        if (rlen <= 12) {
            s_dns_txn_id++;
            continue;
        }

        uint16_t resp_id = (resp[0] << 8) | resp[1];
        if (resp_id != s_dns_txn_id) {
            s_dns_txn_id++;
            continue;
        }
        if (!(resp[2] & 0x80)) {
            s_dns_txn_id++;
            continue;
        }
        if ((resp[3] & 0x0F) != 0) {
            s_dns_txn_id++;
            continue;
        }

        uint16_t ancount = (resp[6] << 8) | resp[7];
        if (ancount == 0) {
            s_dns_txn_id++;
            continue;
        }

        pos = 12;
        pos = skip_name(resp, (uint16_t)rlen, pos);
        if (pos == 0 || pos + 4 > rlen) {
            s_dns_txn_id++;
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
        s_dns_txn_id++;
    }

    kfree(pkt);
    kfree(resp);
    return found;
}
