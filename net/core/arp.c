#include "lwip/arp.h"
#include "console.h"
#include "serial.h"
#include "string.h"

void arp_input(const void *data, uint16_t len) {
    if (len < sizeof(arp_hdr_t)) return;
    
    arp_hdr_t *arp = (arp_hdr_t *)data;
    // Handle ARP Reply/Request
    kprintf("[ARP] Packet from %08x (MAC %02x:%02x...)\n", 
            arp->src_ip, arp->src_mac[0], arp->src_mac[1]);
}

void arp_resolve(uint32_t ip, uint8_t *mac_out) {
    // Stub: Resolve everything to Broadcast or Gateway
    // In a real implementation, check ARP table or send ARP Request
    
    // For now, if local loopback, use 00:00...
    // If Gateway/External, use Gateway MAC (mocked)
    
    // Default to Broadcast for test
    memset(mac_out, 0xFF, 6);
    
    // Mock: Gateway MAC
    if (ip == 0x0100000A) { // 10.0.0.1
        mac_out[0] = 0x52; mac_out[1] = 0x54; mac_out[2] = 0x00;
        mac_out[3] = 0x12; mac_out[4] = 0x34; mac_out[5] = 0x56;
    }
    
    kprintf("[ARP] Resolving %08x -> %02x:%02x:%02x:%02x:%02x:%02x\n",
            ip, mac_out[0], mac_out[1], mac_out[2], mac_out[3], mac_out[4], mac_out[5]);
}
