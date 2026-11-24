// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/ne2000.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "network/net_priv.h"
#include "os/scheduler.h"

#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b) ((int32_t)((a) - (b)) > 0)
#define SEQ_GE(a, b) ((int32_t)((a) - (b)) >= 0)

#define RTO_MIN 250
#define RTO_INIT 1000
#define RTO_MAX 8000
#define MAX_RETRIES 7     /* then the connection is given up */
#define TIME_WAIT_MS 2000 /* (short: the socket table is small) */
#define LINGER_MS 60000   /* released and still not closed: reset */
#define BACKLOG 4         /* connections waiting for tcp_accept */
#define N_OOO 4           /* out-of-order pieces kept */

typedef struct tcp_cb {
    /* sending: tx holds the bytes from snd_una on */
    uint32_t iss, snd_una, snd_nxt, snd_max;
    uint32_t snd_wnd, wl1, wl2; /* the peer's window, and from when */
    uint32_t cwnd, ssthresh;
    uint16_t mss;
    uint8_t *tx;
    uint16_t tx_head, tx_len;
    bool fin_queued, fin_sent;
    /* receiving: rx holds what came in order, then the out-of-order bits */
    uint32_t rcv_nxt;
    uint16_t adv; /* the window last offered */
    uint8_t *rx;
    uint16_t rx_head, rx_len;
    struct {
        uint32_t lo, hi;
    } ooo[N_OOO];
    uint8_t n_ooo;
    bool fin_in; /* a FIN came early: at fin_seq */
    uint32_t fin_seq;
    /* timers (ms) */
    uint32_t rto, srtt, rttvar;
    bool rtt_on;
    uint32_t rtt_seq, rtt_t0;
    bool rexmit_on;
    uint32_t rexmit_at;
    uint8_t retries, dupacks;
    uint32_t persist_at, persist_iv;
    bool recovering; /* after a fast retransmit, until recover is acked */
    uint32_t recover;
    uint32_t state_t0; /* when TIME_WAIT etc. began */
    uint32_t released_at;
    /* a server's connections */
    int8_t parent; /* the listener, until accepted; -1 */
    bool released; /* the program closed it: the stack frees it */
    bool reset;
} tcp_cb_t;

tcp_stats_t g_tcp_stats;

static net_socket_t *sock_at(int i) {
    if (i < 0 || i >= NET_MAX_SOCKETS || !s_socks || !s_socks[i].used ||
        s_socks[i].proto != IP_PROTO_TCP || !s_socks[i].tcb)
        return NULL;
    return &s_socks[i];
}

static uint32_t new_iss(void) {
    static uint32_t n;
    return 0x12345678u + time_millis() * 2654435761u + (n += 64000);
}

// buffers

static void free_sock(net_socket_t *s) {
    if (s->tcb) {
        kfree(s->tcb->tx);
        kfree(s->tcb->rx);
        kfree(s->tcb);
    }
    s->tcb = NULL;
    s->used = false;
    s->tcp_state = TCP_CLOSED;
}

/* a free socket with its buffers (none for a listener), or -1 */
static int new_sock(bool buffers) {
    if (!s_socks)
        return -1;
    int i = 0;
    while (i < NET_MAX_SOCKETS && s_socks[i].used)
        i++;
    if (i == NET_MAX_SOCKETS)
        return -1;
    net_socket_t *s = &s_socks[i];
    memset(s, 0, sizeof(*s));
    tcp_cb_t *t = kzalloc(sizeof(tcp_cb_t));
    if (!t)
        return -1;
    s->tcb = t;
    if (buffers) {
        t->tx = kmalloc(TCP_TXBUF);
        t->rx = kmalloc(TCP_RXBUF);
        if (!t->tx || !t->rx) {
            free_sock(s);
            return -1;
        }
    }
    s->used = true;
    s->proto = IP_PROTO_TCP;
    t->parent = -1;
    t->mss = 536; /* until the other side says */
    t->rto = RTO_INIT;
    t->adv = TCP_RXBUF;
    return i;
}

static uint16_t rx_room(const tcp_cb_t *t) {
    return t->rx ? (uint16_t)(TCP_RXBUF - t->rx_len) : 0;
}

// sending a segment

/* one segment: seq, flags, and n bytes from tx at off (from snd_una) */
static void xmit_raw(const uint8_t mac[6], const uint8_t ip[4], uint16_t sport,
                     uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
                     uint16_t wnd, const tcp_cb_t *t, uint16_t off,
                     uint16_t n) {
    static uint8_t buf[sizeof(tcp_hdr_t) + 4 + TCP_MSS];
    tcp_hdr_t *th = (tcp_hdr_t *)buf;
    uint16_t hl = sizeof(tcp_hdr_t);
    if (flags & TCP_SYN) { /* the largest segment we take */
        buf[hl] = 2, buf[hl + 1] = 4;
        buf[hl + 2] = TCP_MSS >> 8, buf[hl + 3] = TCP_MSS & 0xFF;
        hl += 4;
    }
    th->src_port = net_htons(sport);
    th->dst_port = net_htons(dport);
    th->seq = net_htonl(seq);
    th->ack = net_htonl(flags & TCP_ACK ? ack : 0);
    th->data_off = (uint8_t)((hl / 4) << 4);
    th->flags = flags;
    th->window = net_htons(wnd);
    th->checksum = 0;
    th->urgent = 0;
    for (uint16_t k = 0; k < n; k++) /* (tx is a ring) */
        buf[hl + k] = t->tx[(t->tx_head + off + k) & (TCP_TXBUF - 1)];
    th->checksum =
        net_htons(udp_tcp_checksum(s_ip, ip, IP_PROTO_TCP, buf, hl + n));
    ip_send(mac, ip, IP_PROTO_TCP, buf, hl + n);
    g_tcp_stats.segs_out++;
}

static void xmit(net_socket_t *s, uint32_t seq, uint8_t flags, uint16_t off,
                 uint16_t n) {
    tcp_cb_t *t = s->tcb;
    t->adv = rx_room(t);
    xmit_raw(s->remote_mac, s->remote_ip, s->local_port, s->remote_port, seq,
             t->rcv_nxt, flags, t->adv, t, off, n);
}

static void send_ack(net_socket_t *s) {
    xmit(s, s->tcb->snd_nxt, TCP_ACK, 0, 0);
}

static void timer_start(tcp_cb_t *t) {
    t->rexmit_on = true;
    t->rexmit_at = time_millis() + t->rto;
}

static bool can_send(tcp_state_t st) {
    return st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT ||
           st == TCP_FIN_WAIT1 || st == TCP_CLOSING || st == TCP_LAST_ACK;
}

/* sends what the windows allow */
static void output(net_socket_t *s) {
    tcp_cb_t *t = s->tcb;
    if (!can_send(s->tcp_state))
        return;
    uint32_t now = time_millis();
    for (;;) {
        uint32_t off = t->snd_nxt - t->snd_una;
        /* (limited transmit: a segment more per duplicate ACK, so a loss
         * in a small window still gets its three) */
        uint32_t cw = t->cwnd + (t->recovering ? 0 : t->dupacks * t->mss);
        uint32_t wnd = t->snd_wnd < cw ? t->snd_wnd : cw;
        if (t->snd_wnd == 0 && off == 0 && t->tx_len &&
            SEQ_GE(now, t->persist_at)) {
            wnd = 1; /* the window is shut: probe it with a byte */
            t->persist_at = now + t->persist_iv;
            if (t->persist_iv < 5000)
                t->persist_iv *= 2;
        }
        uint8_t fl = TCP_ACK;
        uint32_t n = 0;
        if (off < t->tx_len) {
            if (off >= wnd)
                break; /* (the window is full) */
            n = t->tx_len - off;
            if (n > wnd - off)
                n = wnd - off;
            if (n > t->mss)
                n = t->mss;
            if (off + n == t->tx_len) {
                fl |= TCP_PSH;
                if (t->fin_queued)
                    fl |= TCP_FIN;
            }
        } else if (t->fin_queued && off == t->tx_len) {
            fl |= TCP_FIN;
        } else {
            break;
        }
        if (!t->rtt_on && t->snd_nxt == t->snd_max) { /* time it (Karn) */
            t->rtt_on = true;
            t->rtt_seq = t->snd_nxt + n;
            t->rtt_t0 = now;
        }
        xmit(s, t->snd_nxt, fl, (uint16_t)off, (uint16_t)n);
        t->snd_nxt += n + (fl & TCP_FIN ? 1 : 0);
        if (fl & TCP_FIN)
            t->fin_sent = true;
        if (SEQ_GT(t->snd_nxt, t->snd_max))
            t->snd_max = t->snd_nxt;
        if (!t->rexmit_on)
            timer_start(t);
    }
}

// receiving

static void abort_sock(net_socket_t *s) {
    s->tcp_state = TCP_CLOSED;
    s->tcb->reset = true;
    s->tcb->rexmit_on = false;
}

static void enter(net_socket_t *s, tcp_state_t st) {
    s->tcp_state = st;
    s->tcb->state_t0 = time_millis();
}

/* the RTT measured: a new RTO (RFC 6298) */
static void rtt_sample(tcp_cb_t *t, uint32_t m) {
    if (!t->srtt) {
        t->srtt = m;
        t->rttvar = m / 2;
    } else {
        uint32_t d = t->srtt > m ? t->srtt - m : m - t->srtt;
        t->rttvar = (3 * t->rttvar + d) / 4;
        t->srtt = (7 * t->srtt + m) / 8;
    }
    t->rto = t->srtt + 4 * t->rttvar;
    if (t->rto < RTO_MIN)
        t->rto = RTO_MIN;
    if (t->rto > RTO_MAX)
        t->rto = RTO_MAX;
}

/* duplicate ACKs that mean a loss: 3, fewer when fewer segments are out
 * and nothing new is to go (early retransmit: then only ones that don't
 * move the window count, as a window update looks the same) */
static uint8_t dup_threshold(const tcp_cb_t *t) {
    uint32_t segs = (t->snd_max - t->snd_una + t->mss - 1) / t->mss;
    bool more = t->snd_nxt - t->snd_una < t->tx_len;
    if (segs >= 4 || more)
        return 3;
    return segs > 1 ? (uint8_t)(segs - 1) : 1;
}

/* the first unacknowledged segment again */
static void resend_first(net_socket_t *s) {
    tcp_cb_t *t = s->tcb;
    uint16_t n = t->tx_len < t->mss ? t->tx_len : t->mss;
    uint8_t f = TCP_ACK | (n == t->tx_len && t->fin_sent ? TCP_FIN : 0);
    if (n || f & TCP_FIN)
        xmit(s, t->snd_una, f, 0, n);
    g_tcp_stats.fast_rexmit++;
}

static void take_ack(net_socket_t *s, uint32_t ack, uint32_t seq, uint16_t wnd,
                     bool pure) {
    tcp_cb_t *t = s->tcb;
    if (SEQ_GT(ack, t->snd_una) && SEQ_LE(ack, t->snd_max)) {
        uint32_t acked = ack - t->snd_una;
        uint32_t data = acked < t->tx_len ? acked : t->tx_len;
        t->tx_head = (uint16_t)((t->tx_head + data) & (TCP_TXBUF - 1));
        t->tx_len -= (uint16_t)data;
        t->snd_una = ack;
        if (SEQ_LT(t->snd_nxt, t->snd_una))
            t->snd_nxt = t->snd_una;
        if (t->rtt_on && SEQ_GE(ack, t->rtt_seq)) {
            t->rtt_on = false;
            rtt_sample(t, time_millis() - t->rtt_t0 + 1);
        }
        if (t->cwnd < t->ssthresh) /* slow start, then a bit per window */
            t->cwnd += t->mss;
        else
            t->cwnd += (uint32_t)t->mss * t->mss / t->cwnd + 1;
        if (t->cwnd > TCP_TXBUF * 2)
            t->cwnd = TCP_TXBUF * 2;
        t->retries = 0;
        t->dupacks = 0;
        if (t->recovering && SEQ_LT(ack, t->recover)) {
            resend_first(s); /* more was lost in that window (NewReno) */
            t->rtt_on = false;
        } else {
            t->recovering = false;
        }
        if (t->snd_una == t->snd_max)
            t->rexmit_on = false;
        else
            timer_start(t);
    } else if (ack == t->snd_una && pure && t->snd_max != t->snd_una &&
               !t->recovering && (wnd == t->snd_wnd || dup_threshold(t) == 3) &&
               ++t->dupacks >= dup_threshold(t)) {
        /* the same ACK again: the segment after it was lost (the window
         * may move: receivers tune it as they go) */
        uint32_t fl = t->snd_max - t->snd_una;
        t->ssthresh = fl / 2 > 2u * t->mss ? fl / 2 : 2u * t->mss;
        t->cwnd = t->ssthresh;
        t->rtt_on = false;
        t->recovering = true;
        t->recover = t->snd_max;
        resend_first(s);
    }
    /* the window: from the newest segment */
    if (SEQ_LT(t->wl1, seq) || (t->wl1 == seq && SEQ_LE(t->wl2, ack))) {
        if (!t->snd_wnd && wnd) {
            t->persist_iv = 500;
            t->retries = 0;
        }
        if (wnd == 0 && t->snd_wnd) {
            t->persist_iv = 500;
            t->persist_at = time_millis() + t->persist_iv;
        }
        t->snd_wnd = wnd;
        t->wl1 = seq;
        t->wl2 = ack;
    }
}

/* the bytes that came: kept in order, early ones put aside */
static void take_data(tcp_cb_t *t, uint32_t seq, const uint8_t *p, uint16_t n) {
    if (SEQ_LT(seq, t->rcv_nxt)) { /* (some we have) */
        uint32_t d = t->rcv_nxt - seq;
        if (d >= n)
            return;
        p += d;
        n -= (uint16_t)d;
        seq = t->rcv_nxt;
    }
    uint32_t off = seq - t->rcv_nxt, room = rx_room(t);
    if (off >= room)
        return;
    if (n > room - off)
        n = (uint16_t)(room - off);
    uint32_t at = t->rx_head + t->rx_len + off;
    for (uint16_t k = 0; k < n; k++)
        t->rx[(at + k) & (TCP_RXBUF - 1)] = p[k];
    uint32_t lo = seq, hi = seq + n;
    if (off == 0) {
        t->rcv_nxt = hi;
        t->rx_len += n;
    } else { /* early: note the piece, joined with any it touches */
        int k = 0;
        while (k < t->n_ooo) {
            if (SEQ_LE(t->ooo[k].lo, hi) && SEQ_GE(t->ooo[k].hi, lo)) {
                if (SEQ_LT(t->ooo[k].lo, lo))
                    lo = t->ooo[k].lo;
                if (SEQ_GT(t->ooo[k].hi, hi))
                    hi = t->ooo[k].hi;
                t->ooo[k] = t->ooo[--t->n_ooo];
                k = 0;
            } else {
                k++;
            }
        }
        if (t->n_ooo < N_OOO) {
            t->ooo[t->n_ooo].lo = lo;
            t->ooo[t->n_ooo].hi = hi;
            t->n_ooo++;
        }
    }
    /* the gap filled: the pieces after it join */
    int k = 0;
    while (k < t->n_ooo) {
        if (SEQ_LE(t->ooo[k].lo, t->rcv_nxt)) {
            if (SEQ_GT(t->ooo[k].hi, t->rcv_nxt)) {
                t->rx_len += (uint16_t)(t->ooo[k].hi - t->rcv_nxt);
                t->rcv_nxt = t->ooo[k].hi;
            }
            t->ooo[k] = t->ooo[--t->n_ooo];
            k = 0;
        } else {
            k++;
        }
    }
}

/* the FIN, once everything before it is here */
static void take_fin(net_socket_t *s) {
    tcp_cb_t *t = s->tcb;
    if (!t->fin_in || t->fin_seq != t->rcv_nxt)
        return;
    t->fin_in = false;
    t->rcv_nxt++;
    bool ours_acked = t->fin_sent && t->snd_una == t->snd_max;
    switch (s->tcp_state) {
    case TCP_ESTABLISHED:
        enter(s, TCP_CLOSE_WAIT);
        break;
    case TCP_FIN_WAIT1:
        enter(s, ours_acked ? TCP_TIME_WAIT : TCP_CLOSING);
        break;
    case TCP_FIN_WAIT2:
        enter(s, TCP_TIME_WAIT);
        break;
    default:
        break;
    }
}

static void refuse(const ip_hdr_t *iph, const tcp_hdr_t *th, uint16_t seglen) {
    if (th->flags & TCP_RST)
        return;
    uint32_t seq = 0, ack = net_ntohl(th->seq) + seglen;
    uint8_t fl = TCP_RST | TCP_ACK;
    if (th->flags & TCP_ACK) {
        seq = net_ntohl(th->ack);
        fl = TCP_RST;
    }
    xmit_raw(s_rx_mac, iph->src, net_ntohs(th->dst_port),
             net_ntohs(th->src_port), seq, ack, fl, 0, NULL, 0, 0);
}

static uint16_t peer_mss(const tcp_hdr_t *th, uint16_t hl) {
    const uint8_t *o = (const uint8_t *)th + sizeof(tcp_hdr_t);
    int n = hl - (int)sizeof(tcp_hdr_t), i = 0;
    while (i < n && o[i] != 0) {
        if (o[i] == 1) {
            i++;
            continue;
        }
        if (i + 1 >= n || o[i + 1] < 2)
            break;
        if (o[i] == 2 && o[i + 1] == 4 && i + 3 < n) {
            uint16_t m = (uint16_t)(o[i + 2] << 8 | o[i + 3]);
            return m < 64 ? 64 : m > TCP_MSS ? TCP_MSS : m;
        }
        i += o[i + 1];
    }
    return 536;
}

static void init_send(tcp_cb_t *t, uint32_t iss, uint16_t mss) {
    t->iss = iss;
    t->snd_una = iss;
    t->snd_nxt = t->snd_max = iss + 1; /* (the SYN) */
    t->mss = mss;
    t->cwnd = 4u * mss;
    t->ssthresh = 0xFFFF;
    t->persist_iv = 500;
}

/* a SYN to a listener: a new connection, waiting for tcp_accept */
static void incoming(int li, const ip_hdr_t *iph, const tcp_hdr_t *th,
                     uint16_t hl) {
    int waiting = 0;
    for (int k = 0; k < NET_MAX_SOCKETS; k++)
        if (sock_at(k) && s_socks[k].tcb->parent == li)
            waiting++;
    int i = waiting < BACKLOG ? new_sock(true) : -1;
    if (i < 0)
        return; /* (busy: the SYN is sent again later) */
    net_socket_t *s = &s_socks[i];
    tcp_cb_t *t = s->tcb;
    t->parent = (int8_t)li;
    s->local_port = s_socks[li].local_port;
    s->remote_port = net_ntohs(th->src_port);
    memcpy(s->remote_ip, iph->src, 4);
    memcpy(s->remote_mac, s_rx_mac, 6); /* (the router's, if it's far) */
    t->rcv_nxt = net_ntohl(th->seq) + 1;
    init_send(t, new_iss(), peer_mss(th, hl));
    t->snd_wnd = net_ntohs(th->window);
    t->wl1 = t->rcv_nxt - 1;
    enter(s, TCP_SYN_RCVD);
    xmit(s, t->iss, TCP_SYN | TCP_ACK, 0, 0);
    timer_start(t);
}

void tcp_input(const ip_hdr_t *iph, const uint8_t *data, uint16_t len) {
    if (len < sizeof(tcp_hdr_t) || !s_socks)
        return;
    g_tcp_stats.segs_in++;
    const tcp_hdr_t *th = (const tcp_hdr_t *)data;
    uint16_t hl = (uint16_t)((th->data_off >> 4) * 4);
    if (hl < sizeof(tcp_hdr_t) || hl > len)
        return;
    uint16_t dport = net_ntohs(th->dst_port), sport = net_ntohs(th->src_port);
    const uint8_t *p = data + hl;
    uint16_t n = (uint16_t)(len - hl);
    uint32_t seq = net_ntohl(th->seq), ack = net_ntohl(th->ack);
    uint8_t fl = th->flags;
    uint16_t seglen =
        (uint16_t)(n + (fl & TCP_SYN ? 1 : 0) + (fl & TCP_FIN ? 1 : 0));

    /* the connection, or else a listener on the port */
    int si = -1, li = -1;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *c = sock_at(i);
        if (!c || c->local_port != dport)
            continue;
        if (c->tcp_state == TCP_LISTEN) {
            if (!c->tcb->released)
                li = i;
        } else if (c->remote_port == sport &&
                   !memcmp(c->remote_ip, iph->src, 4) &&
                   c->tcp_state != TCP_CLOSED) {
            si = i;
            break;
        }
    }
    if (si < 0) {
        if (li >= 0 && (fl & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN)
            incoming(li, iph, th, hl);
        else
            refuse(iph, th, seglen);
        return;
    }
    net_socket_t *s = &s_socks[si];
    tcp_cb_t *t = s->tcb;

    if (s->tcp_state == TCP_SYN_SENT) {
        bool ack_ok = (fl & TCP_ACK) && ack == t->iss + 1;
        if ((fl & TCP_ACK) && !ack_ok) {
            refuse(iph, th, seglen);
            return;
        }
        if (fl & TCP_RST) {
            if (ack_ok)
                abort_sock(s); /* refused */
            return;
        }
        if ((fl & TCP_SYN) && ack_ok) {
            t->rcv_nxt = seq + 1;
            t->mss = peer_mss(th, hl);
            t->cwnd = 4u * t->mss;
            t->snd_una = ack;
            t->snd_wnd = net_ntohs(th->window);
            t->wl1 = seq;
            t->wl2 = ack;
            t->rexmit_on = false;
            t->retries = 0;
            if (t->rtt_on) {
                t->rtt_on = false;
                rtt_sample(t, time_millis() - t->rtt_t0 + 1);
            }
            enter(s, TCP_ESTABLISHED);
            send_ack(s);
        }
        return;
    }

    /* is it in the window? (an old copy: say again what we have) */
    uint16_t room = rx_room(t);
    bool in_win =
        seglen == 0 ? SEQ_GE(seq, t->rcv_nxt) && SEQ_LE(seq, t->rcv_nxt + room)
                    : SEQ_LT(seq, t->rcv_nxt + (room ? room : 1)) &&
                          SEQ_GT(seq + seglen, t->rcv_nxt);
    if (fl & TCP_RST) {
        if (SEQ_GE(seq, t->rcv_nxt) && SEQ_LE(seq, t->rcv_nxt + room)) {
            if (s->tcp_state == TCP_SYN_RCVD && t->parent >= 0)
                free_sock(s); /* (never accepted: gone quietly) */
            else
                abort_sock(s);
        }
        return;
    }
    if (fl & TCP_SYN) {
        if (s->tcp_state == TCP_SYN_RCVD && seq + 1 == t->rcv_nxt)
            xmit(s, t->iss, TCP_SYN | TCP_ACK, 0, 0); /* our answer was lost */
        else if (s->tcp_state != TCP_SYN_RCVD)
            send_ack(s);
        return;
    }
    if (!in_win) {
        if (seglen)
            send_ack(s);
        return;
    }
    if (!(fl & TCP_ACK))
        return;
    if (s->tcp_state == TCP_SYN_RCVD) {
        if (ack != t->iss + 1) {
            refuse(iph, th, seglen);
            return;
        }
        t->snd_una = ack;
        t->rexmit_on = false;
        t->retries = 0;
        t->wl1 = seq - 1; /* (take this segment's window) */
        enter(s, TCP_ESTABLISHED);
    }

    take_ack(s, ack, seq, net_ntohs(th->window), seglen == 0);
    bool fin_acked = t->fin_sent && t->snd_una == t->snd_max;
    switch (s->tcp_state) {
    case TCP_FIN_WAIT1:
        if (fin_acked)
            enter(s, TCP_FIN_WAIT2);
        break;
    case TCP_CLOSING:
        if (fin_acked)
            enter(s, TCP_TIME_WAIT);
        break;
    case TCP_LAST_ACK:
        if (fin_acked) {
            enter(s, TCP_CLOSED);
            return;
        }
        break;
    default:
        break;
    }

    tcp_state_t st = s->tcp_state;
    bool rcv =
        st == TCP_ESTABLISHED || st == TCP_FIN_WAIT1 || st == TCP_FIN_WAIT2;
    if (rcv && n) {
        take_data(t, seq, p, n);
        if (t->released) { /* nobody reads it: let it go */
            t->rx_head = (uint16_t)((t->rx_head + t->rx_len) & (TCP_RXBUF - 1));
            t->rx_len = 0;
        }
    }
    if (rcv && (fl & TCP_FIN)) {
        t->fin_in = true;
        t->fin_seq = seq + n;
    }
    if (rcv)
        take_fin(s);
    if (n || (fl & TCP_FIN))
        send_ack(s); /* (early data too: the duplicate ACK says so) */
    output(s);
}

// timers

void tcp_tick(void) {
    if (!s_socks)
        return;
    uint32_t now = time_millis();
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = sock_at(i);
        if (!s)
            continue;
        tcp_cb_t *t = s->tcb;
        tcp_state_t st = s->tcp_state;
        if (t->parent >= 0 && (!sock_at(t->parent) ||
                               s_socks[t->parent].tcp_state != TCP_LISTEN)) {
            xmit(s, t->snd_nxt, TCP_RST | TCP_ACK, 0, 0); /* never taken */
            free_sock(s);
            continue;
        }
        if (t->rexmit_on && SEQ_GE(now, t->rexmit_at)) {
            bool shut = t->snd_wnd == 0 && can_send(st); /* (probing) */
            if (!shut && ++t->retries > MAX_RETRIES) {
                xmit(s, t->snd_nxt, TCP_RST | TCP_ACK, 0, 0);
                if (t->parent >= 0)
                    free_sock(s);
                else
                    abort_sock(s);
                continue;
            }
            t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
            t->rtt_on = false;
            g_tcp_stats.timeouts++;
            if (st == TCP_SYN_SENT)
                xmit(s, t->iss, TCP_SYN, 0, 0);
            else if (st == TCP_SYN_RCVD)
                xmit(s, t->iss, TCP_SYN | TCP_ACK, 0, 0);
            else {
                uint32_t fl = t->snd_max - t->snd_una;
                t->ssthresh = fl / 2 > 2u * t->mss ? fl / 2 : 2u * t->mss;
                t->cwnd = t->mss;
                t->recovering = false;
                t->snd_nxt = t->snd_una; /* again from the first lost */
                t->rexmit_on = false;
                output(s);
            }
            timer_start(t);
        }
        if (can_send(st) && t->snd_wnd == 0 && t->tx_len &&
            t->snd_una == t->snd_max && SEQ_GE(now, t->persist_at))
            output(s); /* (a window probe) */
        if (st == TCP_TIME_WAIT && now - t->state_t0 >= TIME_WAIT_MS)
            enter(s, TCP_CLOSED);
        if (t->released) {
            bool done =
                s->tcp_state == TCP_CLOSED || s->tcp_state == TCP_LISTEN;
            if (!done && now - t->released_at >= LINGER_MS) {
                xmit(s, t->snd_nxt, TCP_RST | TCP_ACK, 0, 0);
                done = true;
            }
            if (done)
                free_sock(s);
        }
    }
}

// the calls

static uint16_t s_next_port = 49200;

static bool port_used(uint16_t port) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++)
        if (s_socks[i].used && s_socks[i].local_port == port &&
            s_socks[i].proto == IP_PROTO_TCP)
            return true;
    return false;
}

int tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                uint16_t local_port) {
    uint8_t mac[6];
    if (!s_socks || !resolve_mac(dst_ip, mac))
        return -1;
    int i = new_sock(true);
    if (i < 0)
        return -1;
    net_socket_t *s = &s_socks[i];
    tcp_cb_t *t = s->tcb;
    if (!local_port) {
        s->used = false; /* (not itself) */
        for (int k = 0; k < 700 && !local_port; k++) {
            uint16_t p = s_next_port++;
            if (s_next_port >= 49900)
                s_next_port = 49200;
            if (!port_used(p))
                local_port = p;
        }
        s->used = true;
    }
    s->local_port = local_port;
    s->remote_port = dst_port;
    memcpy(s->remote_ip, dst_ip, 4);
    memcpy(s->remote_mac, mac, 6);
    init_send(t, new_iss(), 536);
    enter(s, TCP_SYN_SENT);
    t->rtt_on = true;
    t->rtt_t0 = time_millis();
    xmit(s, t->iss, TCP_SYN, 0, 0);
    timer_start(t);
    uint32_t until = time_millis() + 5000;
    while (s->tcp_state == TCP_SYN_SENT && SEQ_LT(time_millis(), until)) {
        net_poll(); /* (the timer sends the SYN again) */
        task_yield();
    }
    if (s->tcp_state == TCP_ESTABLISHED)
        return i;
    if (s->tcp_state == TCP_SYN_SENT)
        xmit(s, t->iss + 1, TCP_RST, 0, 0);
    free_sock(s);
    return -1;
}

int tcp_listen(uint16_t port) {
    if (!s_socks || !port)
        return -1;
    for (int i = 0; i < NET_MAX_SOCKETS; i++)
        if (sock_at(i) && s_socks[i].local_port == port &&
            s_socks[i].tcp_state == TCP_LISTEN && !s_socks[i].tcb->released)
            return -1; /* (taken) */
    int i = new_sock(false);
    if (i < 0)
        return -1;
    s_socks[i].local_port = port;
    enter(&s_socks[i], TCP_LISTEN);
    return i;
}

int tcp_accept(int li) {
    net_socket_t *l = sock_at(li);
    if (!l || l->tcp_state != TCP_LISTEN)
        return -1;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = sock_at(i);
        if (s && s->tcb->parent == li && s->tcp_state != TCP_SYN_RCVD) {
            s->tcb->parent = -1; /* (it may have closed already) */
            return i;
        }
    }
    return -1;
}

void tcp_release(int sock) {
    net_socket_t *s = sock_at(sock);
    if (!s || s->tcb->released)
        return;
    tcp_cb_t *t = s->tcb;
    t->released = true;
    t->released_at = time_millis();
    t->rx_head = 0; /* (unread: dropped) */
    t->rx_len = 0;
    switch (s->tcp_state) {
    case TCP_ESTABLISHED:
    case TCP_SYN_RCVD:
        t->fin_queued = true;
        enter(s, TCP_FIN_WAIT1);
        break;
    case TCP_CLOSE_WAIT:
        t->fin_queued = true;
        enter(s, TCP_LAST_ACK);
        break;
    case TCP_SYN_SENT:
        enter(s, TCP_CLOSED);
        break;
    default:
        break;
    }
    if (t->rx && rx_room(t) > t->adv) /* (it was full: reopen) */
        send_ack(s);
    output(s);
    if (s->tcp_state == TCP_CLOSED || s->tcp_state == TCP_LISTEN)
        free_sock(s);
}

void tcp_abort(int sock) {
    net_socket_t *s = sock_at(sock);
    if (!s)
        return;
    tcp_state_t st = s->tcp_state;
    if (st != TCP_CLOSED && st != TCP_LISTEN && st != TCP_SYN_SENT &&
        st != TCP_TIME_WAIT)
        xmit(s, s->tcb->snd_nxt, TCP_RST | TCP_ACK, 0, 0);
    free_sock(s);
}

void tcp_close(int sock) {
    net_socket_t *s = sock_at(sock);
    if (!s)
        return;
    tcp_cb_t *t = s->tcb;
    tcp_release(sock);
    uint32_t until = time_millis() + 3000;
    while (s->used && s->tcb == t && SEQ_LT(time_millis(), until) &&
           s->tcp_state != TCP_TIME_WAIT && s->tcp_state != TCP_FIN_WAIT2) {
        net_poll();
        task_yield();
    }
}

int tcp_write(int sock, const uint8_t *data, uint16_t len) {
    net_socket_t *s = sock_at(sock);
    if (!s || s->tcb->released ||
        (s->tcp_state != TCP_ESTABLISHED && s->tcp_state != TCP_CLOSE_WAIT))
        return -1;
    tcp_cb_t *t = s->tcb;
    uint16_t room = (uint16_t)(TCP_TXBUF - t->tx_len);
    uint16_t n = len < room ? len : room;
    uint32_t at = t->tx_head + t->tx_len;
    for (uint16_t k = 0; k < n; k++)
        t->tx[(at + k) & (TCP_TXBUF - 1)] = data[k];
    t->tx_len += n;
    output(s);
    return n;
}

bool tcp_send(int sock, const uint8_t *data, uint16_t len) {
    uint32_t last = time_millis();
    while (len) {
        int n = tcp_write(sock, data, len);
        if (n < 0)
            return false;
        if (n) {
            data += n;
            len -= (uint16_t)n;
            last = time_millis();
            continue;
        }
        if (time_millis() - last > 30000)
            return false; /* (nothing moves) */
        net_poll();
        task_yield();
    }
    return true;
}

int tcp_recv(int sock, uint8_t *buf, uint16_t maxlen) {
    net_socket_t *s = sock_at(sock);
    if (!s || !s->tcb->rx)
        return -1;
    tcp_cb_t *t = s->tcb;
    uint16_t n = t->rx_len < maxlen ? t->rx_len : maxlen;
    for (uint16_t k = 0; k < n; k++)
        buf[k] = t->rx[(t->rx_head + k) & (TCP_RXBUF - 1)];
    t->rx_head = (uint16_t)((t->rx_head + n) & (TCP_RXBUF - 1));
    t->rx_len -= n;
    /* room again: say so (not for every byte) */
    uint16_t room = rx_room(t);
    bool open = s->tcp_state == TCP_ESTABLISHED ||
                s->tcp_state == TCP_FIN_WAIT1 || s->tcp_state == TCP_FIN_WAIT2;
    if (n && open &&
        (room >= t->adv + t->mss || (t->adv < t->mss && room >= t->mss)))
        send_ack(s);
    return n;
}

uint16_t tcp_available(int sock) {
    net_socket_t *s = sock_at(sock);
    return s ? s->tcb->rx_len : 0;
}

uint16_t tcp_pending(int sock) {
    net_socket_t *s = sock_at(sock);
    if (!s)
        return 0;
    tcp_cb_t *t = s->tcb;
    return (uint16_t)(t->tx_len + (t->fin_queued && t->snd_una != t->snd_max));
}

tcp_state_t tcp_state(int sock) {
    net_socket_t *s = sock_at(sock);
    return s ? s->tcp_state : TCP_CLOSED;
}

bool tcp_was_reset(int sock) {
    net_socket_t *s = sock_at(sock);
    return !s || s->tcb->reset;
}

bool tcp_peer(int sock, uint8_t ip[4], uint16_t *port) {
    net_socket_t *s = sock_at(sock);
    if (!s)
        return false;
    if (ip)
        memcpy(ip, s->remote_ip, 4);
    if (port)
        *port = s->remote_port;
    return true;
}

uint16_t tcp_local_port(int sock) {
    net_socket_t *s = sock_at(sock);
    return s ? s->local_port : 0;
}
