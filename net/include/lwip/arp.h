#ifndef LWIP_ARP_H
#define LWIP_ARP_H

#include <stdint.h>
#include "eth.h"

typedef struct {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t hw_len;
    uint8_t proto_len;
    uint16_t opcode;
    uint8_t src_mac[ETH_ADDR_LEN];
    uint32_t src_ip;
    uint8_t dest_mac[ETH_ADDR_LEN];
    uint32_t dest_ip;
} __attribute__((packed)) arp_hdr_t;

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

void arp_input(const void *data, uint16_t len);
void arp_resolve(uint32_t ip, uint8_t *mac_out);

#endif
