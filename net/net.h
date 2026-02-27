#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>

// ── QEMU SLIRP defaults ───────────────────────────────────────────────────────
// Guest IP is statically assigned; no DHCP needed for QEMU user-mode networking
#define NET_IP_ADDR   0x0A00020F   // 10.0.2.15  (host byte order)
#define NET_GATEWAY   0x0A000202   // 10.0.2.2   (QEMU gateway)
#define NET_NETMASK   0xFFFFFF00   // 255.255.255.0
#define NET_DNS       0x0A000203   // 10.0.2.3   (QEMU DNS)

// ── Socket types / address families ──────────────────────────────────────────
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define AF_INET       2

// ── Byte-order (inline, safe to include in C files) ───────────────────────────
static inline uint16_t net_htons(uint16_t v)
    { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t net_ntohs(uint16_t v) { return net_htons(v); }
static inline uint32_t net_htonl(uint32_t v)
    { return ((v&0xFF)<<24)|((v&0xFF00)<<8)|((v&0xFF0000)>>8)|((v>>24)&0xFF); }
static inline uint32_t net_ntohl(uint32_t v) { return net_htonl(v); }

// Note: Use net_htons/net_ntohs/net_htonl/net_ntohl in files that include net.h
// to avoid conflicts with any other htons definitions.

// ── Opaque socket type ────────────────────────────────────────────────────────
typedef struct net_socket net_socket_t;

// ── Stack init / RX entry ─────────────────────────────────────────────────────
void net_init(void);
void net_stack_rx(const void *data, uint16_t len);  // called from E1000 ISR

// ── Socket API (kernel-internal) ──────────────────────────────────────────────
net_socket_t *net_socket_create(int type);
int           net_socket_bind(net_socket_t *s, uint32_t ip, uint16_t port);
int           net_socket_listen(net_socket_t *s, int backlog);
int           net_socket_connect(net_socket_t *s, uint32_t ip, uint16_t port);
net_socket_t *net_socket_accept(net_socket_t *s,
                                uint32_t *remote_ip, uint16_t *remote_port);
int           net_socket_send(net_socket_t *s, const void *buf, int len);
int           net_socket_recv(net_socket_t *s, void *buf, int len, int flags);
void          net_socket_close(net_socket_t *s);
int           net_socket_rx_avail(net_socket_t *s);
int           net_socket_is_connected(net_socket_t *s);
int           net_socket_get_type(net_socket_t *s);  // SOCK_STREAM or SOCK_DGRAM

// Field accessors (avoid exposing incomplete struct internals)
void     net_socket_set_remote(net_socket_t *s, uint32_t ip, uint16_t port);
uint32_t net_socket_get_local_ip(net_socket_t *s);
uint16_t net_socket_get_local_port(net_socket_t *s);
uint32_t net_socket_get_remote_ip(net_socket_t *s);
uint16_t net_socket_get_remote_port(net_socket_t *s);

// ── BSD sockaddr_in (for syscall layer) ───────────────────────────────────────
typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;    // network byte order
    uint32_t sin_addr;    // network byte order
    uint8_t  sin_zero[8];
} sockaddr_in_t;

#endif // NET_H
