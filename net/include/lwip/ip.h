#ifndef LWIP_IP_H
#define LWIP_IP_H

#include "arch.h"
#include "pbuf.h"
#include "netif.h"

// IP Header
struct ip_hdr {
    u8_t _v_hl;
    u8_t _tos;
    u16_t _len;
    u16_t _id;
    u16_t _offset;
    u8_t _ttl;
    u8_t _proto;
    u16_t _chksum;
    u32_t src;
    u32_t dest;
} __attribute__((packed));

#define IP_HL(hdr)  ((hdr)->_v_hl & 0x0f)
#define IP_V(hdr)   ((hdr)->_v_hl >> 4)

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

void ip_input(struct pbuf *p, struct netif *inp);
err_t ip_output(struct pbuf *p, u32_t src, u32_t dest, u8_t ttl, u8_t proto);

#endif
