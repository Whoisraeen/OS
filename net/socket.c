// net/socket.c — Minimal functional IP/ARP/ICMP/TCP/UDP stack
// Targets QEMU SLIRP (static IP 10.0.2.15, GW 10.0.2.2)

#include "net.h"
#include "../spinlock.h"
#include "../sched.h"
#include "../heap.h"
#include "../string.h"
#include "../serial.h"
#include "../rtc.h"
#include "../drivers/e1000.h"

// ── Byte-order helpers ────────────────────────────────────────────────────────
static inline uint16_t htons(uint16_t v)
    { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
    { return ((v&0xFF)<<24)|((v&0xFF00)<<8)|((v&0xFF0000)>>8)|((v>>24)&0xFF); }
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

// ── Packet headers ────────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  dst[6], src[6];
    uint16_t type;
} eth_hdr_t;
#define ETHTYPE_IP  0x0800
#define ETHTYPE_ARP 0x0806

typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} arp_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl, tos;
    uint16_t total_len, id, flags_frag;
    uint8_t  ttl, proto;
    uint16_t checksum;
    uint32_t src, dst;
} ip4_hdr_t;
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct __attribute__((packed)) {
    uint8_t  type, code;
    uint16_t checksum, id, seq;
} icmp_echo_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t  data_off, flags;
    uint16_t window, checksum, urgent;
} tcp_hdr_t;
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port, length, checksum;
} udp_hdr_t;

// Pseudo-header for TCP/UDP checksum
typedef struct __attribute__((packed)) {
    uint32_t src, dst;
    uint8_t  zero, proto;
    uint16_t length;
} ip_pseudo_t;

// ── Checksum ──────────────────────────────────────────────────────────────────
static uint16_t ip_checksum(const void *data, size_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t transport_checksum(uint32_t src, uint32_t dst,
                                   uint8_t proto,
                                   const void *hdr, size_t hdr_len,
                                   const void *data, size_t data_len)
{
    ip_pseudo_t ph;
    ph.src    = htonl(src);
    ph.dst    = htonl(dst);
    ph.zero   = 0;
    ph.proto  = proto;
    ph.length = htons((uint16_t)(hdr_len + data_len));

    // Sum pseudo + header + data using a scratch buffer
    // Max practical packet: 1500 - 20 (IP) = 1480
    uint8_t scratch[1600];
    size_t off = 0;
    memcpy(scratch + off, &ph,  sizeof(ph));  off += sizeof(ph);
    memcpy(scratch + off, hdr,  hdr_len);     off += hdr_len;
    if (data && data_len) { memcpy(scratch + off, data, data_len); off += data_len; }
    return ip_checksum(scratch, off);
}

// ── Network config ────────────────────────────────────────────────────────────
static uint32_t our_ip   = NET_IP_ADDR;
static uint32_t our_gw   = NET_GATEWAY;
static uint32_t our_mask = NET_NETMASK;
static uint8_t  our_mac[6];
static uint16_t ip_id_counter = 1;

// ── ARP cache ─────────────────────────────────────────────────────────────────
#define ARP_CACHE_SIZE 16
typedef struct { uint32_t ip; uint8_t mac[6]; int valid; } arp_entry_t;
static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static spinlock_t  arp_lock;

// ARP wait: one pending resolution at a time
static task_t    *arp_waiter;
static uint32_t   arp_wait_ip;
static uint8_t    arp_wait_mac[6];
static int        arp_wait_done;

static uint8_t *arp_lookup(uint32_t ip)
{
    spinlock_acquire(&arp_lock);
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            spinlock_release(&arp_lock);
            return arp_cache[i].mac;
        }
    }
    spinlock_release(&arp_lock);
    return NULL;
}

static void arp_store(uint32_t ip, const uint8_t *mac)
{
    spinlock_acquire(&arp_lock);
    // Find existing or free slot
    int slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].ip == ip && arp_cache[i].valid) { slot = i; break; }
        if (!arp_cache[i].valid && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;  // Evict first
    arp_cache[slot].ip    = ip;
    arp_cache[slot].valid = 1;
    memcpy(arp_cache[slot].mac, mac, 6);
    spinlock_release(&arp_lock);
}

// ── Send raw Ethernet frame ───────────────────────────────────────────────────
static void eth_send(const uint8_t *dst_mac, uint16_t type,
                     const void *payload, size_t len)
{
    uint8_t frame[1514];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, our_mac, 6);
    eth->type = htons(type);
    if (len > 1500) len = 1500;
    memcpy(frame + sizeof(eth_hdr_t), payload, len);
    e1000_send_packet(frame, (uint16_t)(sizeof(eth_hdr_t) + len));
}

// ── ARP request/reply ─────────────────────────────────────────────────────────
static void arp_send_request(uint32_t target_ip)
{
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t zero_mac[6] = {0};
    arp_pkt_t pkt;
    pkt.htype = htons(1);
    pkt.ptype = htons(0x0800);
    pkt.hlen  = 6;
    pkt.plen  = 4;
    pkt.op    = htons(1);  // request
    memcpy(pkt.sha, our_mac, 6);
    pkt.spa = htonl(our_ip);
    memcpy(pkt.tha, zero_mac, 6);
    pkt.tpa = htonl(target_ip);
    eth_send(bcast, ETHTYPE_ARP, &pkt, sizeof(pkt));
}

static void arp_send_reply(uint32_t dst_ip, const uint8_t *dst_mac)
{
    arp_pkt_t pkt;
    pkt.htype = htons(1);
    pkt.ptype = htons(0x0800);
    pkt.hlen  = 6;
    pkt.plen  = 4;
    pkt.op    = htons(2);  // reply
    memcpy(pkt.sha, our_mac, 6);
    pkt.spa = htonl(our_ip);
    memcpy(pkt.tha, dst_mac, 6);
    pkt.tpa = htonl(dst_ip);
    eth_send(dst_mac, ETHTYPE_ARP, &pkt, sizeof(pkt));
}

// Resolve IP → MAC (blocks if not in cache, up to ~1s)
static const uint8_t *arp_resolve(uint32_t ip)
{
    // Route to gateway if not on local subnet
    uint32_t next_hop = ((ip & our_mask) == (our_ip & our_mask)) ? ip : our_gw;

    uint8_t *cached = arp_lookup(next_hop);
    if (cached) return cached;

    // Send ARP request and wait
    arp_wait_ip   = next_hop;
    arp_wait_done = 0;
    arp_waiter    = task_get_by_id(task_current_id());
    arp_send_request(next_hop);

    // Poll-wait (simple: busy-wait with yields up to 500ms)
    for (int i = 0; i < 50 && !arp_wait_done; i++) {
        task_yield();
    }
    arp_waiter = NULL;

    if (arp_wait_done) {
        arp_store(next_hop, arp_wait_mac);
        return arp_lookup(next_hop);
    }
    return NULL;
}

// ── IP send ───────────────────────────────────────────────────────────────────
static void ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    uint8_t pkt[1500];
    ip4_hdr_t *iph = (ip4_hdr_t *)pkt;
    size_t total = sizeof(ip4_hdr_t) + len;
    if (total > 1500) total = 1500;

    iph->ver_ihl   = 0x45;
    iph->tos       = 0;
    iph->total_len = htons((uint16_t)total);
    iph->id        = htons(ip_id_counter++);
    iph->flags_frag = 0;
    iph->ttl       = 64;
    iph->proto     = proto;
    iph->checksum  = 0;
    iph->src       = htonl(our_ip);
    iph->dst       = htonl(dst);
    iph->checksum  = ip_checksum(iph, sizeof(ip4_hdr_t));

    memcpy(pkt + sizeof(ip4_hdr_t), payload, len);

    const uint8_t *dst_mac = arp_resolve(dst);
    if (!dst_mac) {
        kprintf("[NET] ARP failed for %u.%u.%u.%u\n",
                (dst>>24)&0xFF,(dst>>16)&0xFF,(dst>>8)&0xFF,dst&0xFF);
        return;
    }
    eth_send(dst_mac, ETHTYPE_IP, pkt, total);
}

// ── ICMP handler ──────────────────────────────────────────────────────────────
static void handle_icmp(uint32_t src_ip, const void *data, size_t len)
{
    if (len < sizeof(icmp_echo_t)) return;
    const icmp_echo_t *req = (const icmp_echo_t *)data;
    if (req->type != 8) return;  // Only echo-request

    uint8_t reply_buf[1024];
    icmp_echo_t *rep = (icmp_echo_t *)reply_buf;
    rep->type     = 0;  // echo-reply
    rep->code     = 0;
    rep->checksum = 0;
    rep->id       = req->id;
    rep->seq      = req->seq;
    size_t payload_len = len - sizeof(icmp_echo_t);
    if (payload_len + sizeof(icmp_echo_t) > sizeof(reply_buf)) payload_len = 0;
    memcpy(reply_buf + sizeof(icmp_echo_t),
           (const uint8_t*)data + sizeof(icmp_echo_t), payload_len);
    rep->checksum = ip_checksum(reply_buf, sizeof(icmp_echo_t) + payload_len);
    ip_send(src_ip, IP_PROTO_ICMP, reply_buf, sizeof(icmp_echo_t) + payload_len);
}

// ── TCP state machine constants ───────────────────────────────────────────────
#define TCP_CLOSED      0
#define TCP_LISTEN      1
#define TCP_SYN_SENT    2
#define TCP_SYN_RECV    3
#define TCP_ESTABLISHED 4
#define TCP_FIN_WAIT1   5
#define TCP_FIN_WAIT2   6
#define TCP_CLOSE_WAIT  7
#define TCP_LAST_ACK    8
#define TCP_TIME_WAIT   9

// ── Socket table ─────────────────────────────────────────────────────────────
#define MAX_SOCKETS    16
#define SOCK_RX_SIZE   65536
#define ACCEPT_BACKLOG 8

struct net_socket {
    int       used;
    int       type;        // SOCK_STREAM or SOCK_DGRAM

    // Addressing
    uint32_t  local_ip;
    uint16_t  local_port;
    uint32_t  remote_ip;
    uint16_t  remote_port;

    // TCP state
    int       tcp_state;
    uint32_t  snd_nxt;    // next seq to send
    uint32_t  snd_una;    // oldest unacked seq
    uint32_t  rcv_nxt;    // next expected seq from remote
    uint16_t  snd_wnd;    // remote window

    // RX ring buffer (thread-safe)
    uint8_t   rx_buf[SOCK_RX_SIZE];
    uint32_t  rx_head;    // read position
    uint32_t  rx_tail;    // write position (ISR)
    spinlock_t rx_lock;

    // Blocking tasks
    task_t   *rx_waiter;       // blocked in recv()
    task_t   *connect_waiter;  // blocked in connect()
    task_t   *accept_waiter;   // blocked in accept()

    // Accept queue (for LISTEN sockets)
    struct net_socket *accept_q[ACCEPT_BACKLOG];
    int accept_head, accept_tail;
    spinlock_t accept_lock;
};

static struct net_socket sockets[MAX_SOCKETS];
static spinlock_t socket_table_lock;
static uint16_t   ephemeral_port = 49152;

static struct net_socket *sock_alloc(void)
{
    spinlock_acquire(&socket_table_lock);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].used = 1;
            spinlock_init(&sockets[i].rx_lock);
            spinlock_init(&sockets[i].accept_lock);
            spinlock_release(&socket_table_lock);
            return &sockets[i];
        }
    }
    spinlock_release(&socket_table_lock);
    return NULL;
}

static void sock_rx_push(struct net_socket *s, const uint8_t *data, size_t len)
{
    spinlock_acquire(&s->rx_lock);
    for (size_t i = 0; i < len; i++) {
        uint32_t next = (s->rx_tail + 1) % SOCK_RX_SIZE;
        if (next == s->rx_head) break;  // Buffer full
        s->rx_buf[s->rx_tail] = data[i];
        s->rx_tail = next;
    }
    task_t *waiter = s->rx_waiter;
    s->rx_waiter = NULL;
    spinlock_release(&s->rx_lock);
    if (waiter) task_unblock(waiter);
}

static uint32_t sock_rx_avail(struct net_socket *s)
{
    spinlock_acquire(&s->rx_lock);
    uint32_t n = (s->rx_tail - s->rx_head + SOCK_RX_SIZE) % SOCK_RX_SIZE;
    spinlock_release(&s->rx_lock);
    return n;
}

static struct net_socket *find_tcp_socket(uint32_t src_ip, uint16_t src_port,
                                           uint16_t dst_port)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        struct net_socket *s = &sockets[i];
        if (!s->used || s->type != SOCK_STREAM) continue;
        if (s->local_port != dst_port) continue;
        if (s->tcp_state == TCP_LISTEN) return s;
        if (s->remote_ip == src_ip && s->remote_port == src_port) return s;
    }
    return NULL;
}

static struct net_socket *find_udp_socket(uint16_t dst_port)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        struct net_socket *s = &sockets[i];
        if (s->used && s->type == SOCK_DGRAM && s->local_port == dst_port)
            return s;
    }
    return NULL;
}

// ── TCP send ──────────────────────────────────────────────────────────────────
static void tcp_send_segment(struct net_socket *s, uint8_t flags,
                              const void *data, size_t data_len)
{
    uint8_t seg[sizeof(tcp_hdr_t) + 1400];
    tcp_hdr_t *tcp = (tcp_hdr_t *)seg;
    tcp->src_port  = htons(s->local_port);
    tcp->dst_port  = htons(s->remote_port);
    tcp->seq       = htonl(s->snd_nxt);
    tcp->ack       = (flags & TCP_ACK) ? htonl(s->rcv_nxt) : 0;
    tcp->data_off  = (5 << 4);  // 20-byte header
    tcp->flags     = flags;
    tcp->window    = htons(8192);
    tcp->checksum  = 0;
    tcp->urgent    = 0;

    if (data && data_len > 1400) data_len = 1400;
    if (data && data_len)
        memcpy(seg + sizeof(tcp_hdr_t), data, data_len);

    tcp->checksum = transport_checksum(our_ip, s->remote_ip, IP_PROTO_TCP,
                                       tcp, sizeof(tcp_hdr_t),
                                       data, data_len);

    ip_send(s->remote_ip, IP_PROTO_TCP, seg, sizeof(tcp_hdr_t) + data_len);

    if (flags & (TCP_SYN | TCP_FIN))
        s->snd_nxt++;
    if (data_len)
        s->snd_nxt += (uint32_t)data_len;
}

// ── TCP input ─────────────────────────────────────────────────────────────────
static void handle_tcp(uint32_t src_ip, const void *data, size_t len)
{
    if (len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *tcp = (const tcp_hdr_t *)data;

    uint16_t src_port  = ntohs(tcp->src_port);
    uint16_t dst_port  = ntohs(tcp->dst_port);
    uint8_t  flags     = tcp->flags;
    uint32_t seq       = ntohl(tcp->seq);
    uint32_t ack       = ntohl(tcp->ack);
    size_t   hdr_len   = (tcp->data_off >> 4) * 4;
    const uint8_t *payload = (const uint8_t *)data + hdr_len;
    size_t   pay_len   = (len > hdr_len) ? len - hdr_len : 0;

    struct net_socket *s = find_tcp_socket(src_ip, src_port, dst_port);
    if (!s) return;  // No socket, drop

    switch (s->tcp_state) {

    case TCP_LISTEN:
        if (!(flags & TCP_SYN)) return;
        {
            // Allocate child socket
            struct net_socket *child = sock_alloc();
            if (!child) {
                // Send RST
                return;
            }
            child->type        = SOCK_STREAM;
            child->local_ip    = our_ip;
            child->local_port  = dst_port;
            child->remote_ip   = src_ip;
            child->remote_port = src_port;
            child->rcv_nxt     = seq + 1;
            child->snd_nxt     = (uint32_t)(rtc_get_timestamp() * 37 + 12345);
            child->tcp_state   = TCP_SYN_RECV;

            // Send SYN-ACK
            tcp_send_segment(child, TCP_SYN | TCP_ACK, NULL, 0);
            child->tcp_state = TCP_SYN_RECV;

            // Enqueue in parent's accept queue
            spinlock_acquire(&s->accept_lock);
            int next = (s->accept_tail + 1) % ACCEPT_BACKLOG;
            if (next != s->accept_head) {
                s->accept_q[s->accept_tail] = child;
                s->accept_tail = next;
            } else {
                child->used = 0;  // Queue full, drop
            }
            task_t *waiter = s->accept_waiter;
            s->accept_waiter = NULL;
            spinlock_release(&s->accept_lock);
            if (waiter) task_unblock(waiter);
        }
        break;

    case TCP_SYN_RECV:
        if ((flags & TCP_ACK) && !(flags & TCP_SYN)) {
            s->snd_una     = ack;
            s->tcp_state   = TCP_ESTABLISHED;
            // If parent accept_waiter, wake
        }
        break;

    case TCP_SYN_SENT:
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            s->rcv_nxt   = seq + 1;
            s->snd_una   = ack;
            s->tcp_state = TCP_ESTABLISHED;
            // Send ACK
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            task_t *w = s->connect_waiter;
            s->connect_waiter = NULL;
            if (w) task_unblock(w);
        } else if (flags & TCP_RST) {
            s->tcp_state = TCP_CLOSED;
            task_t *w = s->connect_waiter;
            s->connect_waiter = NULL;
            if (w) task_unblock(w);
        }
        break;

    case TCP_ESTABLISHED:
        s->snd_una = ack;
        if (flags & TCP_RST) {
            s->tcp_state = TCP_CLOSED;
            task_t *w = s->rx_waiter;
            s->rx_waiter = NULL;
            if (w) task_unblock(w);
            break;
        }
        if (pay_len > 0) {
            s->rcv_nxt += (uint32_t)pay_len;
            sock_rx_push(s, payload, pay_len);
            tcp_send_segment(s, TCP_ACK, NULL, 0);
        }
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            tcp_send_segment(s, TCP_FIN | TCP_ACK, NULL, 0);
            s->tcp_state = TCP_LAST_ACK;
            // Unblock any rx waiter with EOF
            task_t *w = s->rx_waiter;
            s->rx_waiter = NULL;
            if (w) task_unblock(w);
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_ACK) {
            s->snd_una   = ack;
            s->tcp_state = TCP_FIN_WAIT2;
        }
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            s->tcp_state = TCP_TIME_WAIT;
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            s->tcp_state = TCP_TIME_WAIT;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) {
            s->tcp_state = TCP_CLOSED;
            s->used = 0;
        }
        break;

    default:
        break;
    }
}

// ── UDP input ─────────────────────────────────────────────────────────────────
static void handle_udp(uint32_t src_ip, const void *data, size_t len)
{
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t *udp = (const udp_hdr_t *)data;
    uint16_t dst_port = ntohs(udp->dst_port);
    const uint8_t *payload = (const uint8_t *)data + sizeof(udp_hdr_t);
    size_t pay_len = ntohs(udp->length);
    if (pay_len < sizeof(udp_hdr_t)) return;
    pay_len -= sizeof(udp_hdr_t);
    if (pay_len + sizeof(udp_hdr_t) > len) pay_len = len - sizeof(udp_hdr_t);

    struct net_socket *s = find_udp_socket(dst_port);
    if (!s) return;

    s->remote_ip   = src_ip;
    s->remote_port = ntohs(udp->src_port);
    sock_rx_push(s, payload, pay_len);
}

// ── ARP input ─────────────────────────────────────────────────────────────────
static void handle_arp(const void *data, size_t len)
{
    if (len < sizeof(arp_pkt_t)) return;
    const arp_pkt_t *arp = (const arp_pkt_t *)data;
    uint16_t op   = ntohs(arp->op);
    uint32_t spa  = ntohl(arp->spa);
    uint32_t tpa  = ntohl(arp->tpa);

    // Learn sender MAC
    arp_store(spa, arp->sha);

    if (op == 1 && tpa == our_ip) {
        // ARP request for us: reply
        arp_send_reply(spa, arp->sha);
    } else if (op == 2) {
        // ARP reply: wake up anyone waiting
        if (arp_waiter && spa == arp_wait_ip) {
            memcpy(arp_wait_mac, arp->sha, 6);
            arp_wait_done = 1;
            task_t *w = arp_waiter;
            arp_waiter = NULL;
            task_unblock(w);
        }
    }
}

// ── Main RX entry (called from E1000 ISR) ────────────────────────────────────
void net_stack_rx(const void *data, uint16_t len)
{
    if (len < (uint16_t)sizeof(eth_hdr_t)) return;
    const eth_hdr_t *eth = (const eth_hdr_t *)data;
    uint16_t type = ntohs(eth->type);
    const uint8_t *payload = (const uint8_t *)data + sizeof(eth_hdr_t);
    size_t pay_len = (size_t)(len - sizeof(eth_hdr_t));

    if (type == ETHTYPE_ARP) {
        handle_arp(payload, pay_len);
    } else if (type == ETHTYPE_IP) {
        if (pay_len < sizeof(ip4_hdr_t)) return;
        const ip4_hdr_t *iph = (const ip4_hdr_t *)payload;
        uint8_t  proto    = iph->proto;
        uint32_t src_ip   = ntohl(iph->src);
        uint32_t dst_ip   = ntohl(iph->dst);
        size_t   ip_hlen  = (iph->ver_ihl & 0x0F) * 4;
        const uint8_t *ip_payload = payload + ip_hlen;
        size_t   ip_plen  = ntohs(iph->total_len);
        if (ip_plen < ip_hlen || ip_plen > pay_len) ip_plen = pay_len;
        ip_plen -= ip_hlen;

        // Only accept unicast to us or broadcast
        if (dst_ip != our_ip && dst_ip != 0xFFFFFFFF) return;

        if (proto == IP_PROTO_ICMP)
            handle_icmp(src_ip, ip_payload, ip_plen);
        else if (proto == IP_PROTO_TCP)
            handle_tcp(src_ip, ip_payload, ip_plen);
        else if (proto == IP_PROTO_UDP)
            handle_udp(src_ip, ip_payload, ip_plen);
    }
}

// ── Public socket API ─────────────────────────────────────────────────────────

void net_init(void)
{
    memset(sockets, 0, sizeof(sockets));
    memset(arp_cache, 0, sizeof(arp_cache));
    spinlock_init(&socket_table_lock);
    spinlock_init(&arp_lock);
    arp_waiter    = NULL;
    arp_wait_done = 0;

    uint8_t *mac = e1000_get_mac();
    memcpy(our_mac, mac, 6);

    // Register our RX handler
    e1000_set_rx_callback(net_stack_rx);

    kprintf("[NET] Stack ready: %u.%u.%u.%u MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            (our_ip>>24)&0xFF, (our_ip>>16)&0xFF, (our_ip>>8)&0xFF, our_ip&0xFF,
            our_mac[0], our_mac[1], our_mac[2],
            our_mac[3], our_mac[4], our_mac[5]);
}

net_socket_t *net_socket_create(int type)
{
    struct net_socket *s = sock_alloc();
    if (!s) return NULL;
    s->type      = type;
    s->local_ip  = our_ip;
    s->tcp_state = TCP_CLOSED;
    return s;
}

int net_socket_bind(net_socket_t *s, uint32_t ip, uint16_t port)
{
    if (!s) return -1;
    s->local_ip   = ip ? ip : our_ip;
    s->local_port = port;
    return 0;
}

int net_socket_listen(net_socket_t *s, int backlog)
{
    (void)backlog;
    if (!s || s->type != SOCK_STREAM) return -1;
    s->tcp_state  = TCP_LISTEN;
    s->accept_head = s->accept_tail = 0;
    return 0;
}

int net_socket_connect(net_socket_t *s, uint32_t ip, uint16_t port)
{
    if (!s || s->type != SOCK_STREAM) return -1;
    if (!s->local_port) {
        s->local_port = ephemeral_port++;
        if (ephemeral_port > 65000) ephemeral_port = 49152;
    }
    s->remote_ip   = ip;
    s->remote_port = port;
    s->snd_nxt     = (uint32_t)(rtc_get_timestamp() * 37 + 99991);
    s->rcv_nxt     = 0;
    s->tcp_state   = TCP_SYN_SENT;

    // Send SYN (arp_resolve may block here)
    tcp_send_segment(s, TCP_SYN, NULL, 0);

    // Block waiting for SYN-ACK
    s->connect_waiter = task_get_by_id(task_current_id());
    for (int i = 0; i < 300 && s->tcp_state == TCP_SYN_SENT; i++)
        task_yield();

    s->connect_waiter = NULL;
    return (s->tcp_state == TCP_ESTABLISHED) ? 0 : -1;
}

net_socket_t *net_socket_accept(net_socket_t *s,
                                uint32_t *remote_ip, uint16_t *remote_port)
{
    if (!s || s->tcp_state != TCP_LISTEN) return NULL;

    // Wait for a connection in the accept queue
    while (1) {
        spinlock_acquire(&s->accept_lock);
        if (s->accept_head != s->accept_tail) {
            struct net_socket *child = s->accept_q[s->accept_head];
            s->accept_head = (s->accept_head + 1) % ACCEPT_BACKLOG;
            spinlock_release(&s->accept_lock);
            if (remote_ip)   *remote_ip   = child->remote_ip;
            if (remote_port) *remote_port = child->remote_port;
            return child;
        }
        s->accept_waiter = task_get_by_id(task_current_id());
        spinlock_release(&s->accept_lock);
        task_block();
        s->accept_waiter = NULL;
    }
}

int net_socket_send(net_socket_t *s, const void *buf, int len)
{
    if (!s || len <= 0) return -1;

    if (s->type == SOCK_STREAM) {
        if (s->tcp_state != TCP_ESTABLISHED) return -1;
        // Segment data into MSS-sized chunks (up to 1400 bytes each)
        const uint8_t *ptr = (const uint8_t *)buf;
        int sent = 0;
        while (sent < len) {
            int chunk = len - sent;
            if (chunk > 1400) chunk = 1400;
            tcp_send_segment(s, TCP_ACK | TCP_PSH, ptr + sent, (size_t)chunk);
            sent += chunk;
        }
        return sent;
    } else {
        // UDP
        uint8_t udp_buf[sizeof(udp_hdr_t) + 1400];
        if (len > 1400) len = 1400;
        udp_hdr_t *udp   = (udp_hdr_t *)udp_buf;
        udp->src_port    = htons(s->local_port);
        udp->dst_port    = htons(s->remote_port);
        udp->length      = htons((uint16_t)(sizeof(udp_hdr_t) + len));
        udp->checksum    = 0;  // Optional for UDP
        memcpy(udp_buf + sizeof(udp_hdr_t), buf, (size_t)len);
        ip_send(s->remote_ip, IP_PROTO_UDP, udp_buf, sizeof(udp_hdr_t) + (size_t)len);
        return len;
    }
}

int net_socket_recv(net_socket_t *s, void *buf, int len, int flags)
{
    if (!s || len <= 0) return -1;
    (void)flags;

    // Block until data is available or socket closed
    while (sock_rx_avail(s) == 0) {
        if (s->tcp_state == TCP_CLOSED ||
            s->tcp_state == TCP_TIME_WAIT ||
            s->tcp_state == TCP_LAST_ACK)
            return 0;  // EOF

        s->rx_waiter = task_get_by_id(task_current_id());
        task_block();
        s->rx_waiter = NULL;
    }

    // Copy from ring buffer
    spinlock_acquire(&s->rx_lock);
    int count = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (count < len && s->rx_head != s->rx_tail) {
        dst[count++] = s->rx_buf[s->rx_head];
        s->rx_head = (s->rx_head + 1) % SOCK_RX_SIZE;
    }
    spinlock_release(&s->rx_lock);
    return count;
}

void net_socket_close(net_socket_t *s)
{
    if (!s || !s->used) return;
    if (s->type == SOCK_STREAM && s->tcp_state == TCP_ESTABLISHED) {
        tcp_send_segment(s, TCP_FIN | TCP_ACK, NULL, 0);
        s->tcp_state = TCP_FIN_WAIT1;
        // Wait briefly for TIME_WAIT
        for (int i = 0; i < 20 && s->tcp_state == TCP_FIN_WAIT1; i++)
            task_yield();
    }
    s->tcp_state = TCP_CLOSED;
    s->used = 0;
}

int net_socket_rx_avail(net_socket_t *s)
{
    return s ? (int)sock_rx_avail(s) : 0;
}

int net_socket_is_connected(net_socket_t *s)
{
    return s && s->tcp_state == TCP_ESTABLISHED;
}

int net_socket_get_type(net_socket_t *s)
{
    return s ? s->type : 0;
}

// ── Field accessors ────────────────────────────────────────────────────────────
void net_socket_set_remote(net_socket_t *s, uint32_t ip, uint16_t port)
    { if (s) { s->remote_ip = ip; s->remote_port = port; } }
uint32_t net_socket_get_local_ip(net_socket_t *s)
    { return s ? s->local_ip : 0; }
uint16_t net_socket_get_local_port(net_socket_t *s)
    { return s ? s->local_port : 0; }
uint32_t net_socket_get_remote_ip(net_socket_t *s)
    { return s ? s->remote_ip : 0; }
uint16_t net_socket_get_remote_port(net_socket_t *s)
    { return s ? s->remote_port : 0; }
