#ifndef LWIP_ETH_H
#define LWIP_ETH_H

#include <stdint.h>

#define ETH_ADDR_LEN 6

typedef struct {
    uint8_t dest[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

#define ETHTYPE_ARP 0x0806
#define ETHTYPE_IP  0x0800

#endif
