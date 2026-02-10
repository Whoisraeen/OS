#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/endian.h"
#include "lwip/arp.h"
#include "console.h"
#include "serial.h"
#include "drivers/e1000.h"

extern void tcp_input(struct pbuf *p, struct netif *inp);

void ip_input(struct pbuf *p, struct netif *inp) {
    if (p->len < sizeof(struct ip_hdr)) {
        pbuf_free(p);
        return;
    }
    
    struct ip_hdr *ip = (struct ip_hdr *)p->payload;
    if (IP_V(ip) != 4) {
        pbuf_free(p);
        return;
    }
    
    // Check protocol
    if (ip->_proto == IP_PROTO_TCP) {
        // Adjust pbuf to point to TCP payload (IP header size)
        uint16_t hlen = IP_HL(ip) * 4;
        // pbuf_header(p, -hlen); // Move forward
        // Manually for mock:
        p->payload = (uint8_t*)p->payload + hlen;
        p->len -= hlen;
        
        tcp_input(p, inp);
    } else {
        pbuf_free(p);
    }
}

err_t ip_output(struct pbuf *p, u32_t src, u32_t dest, u8_t ttl, u8_t proto) {
    // 1. Resolve MAC
    uint8_t dest_mac[6];
    arp_resolve(dest, dest_mac);
    
    // 2. Encapsulate in Ethernet
    eth_hdr_t eth;
    memcpy(eth.dest, dest_mac, 6);
    memcpy(eth.src, e1000_get_mac(), 6);
    eth.type = htons(ETHTYPE_IP);
    
    // 3. Prepend IP header
    // ...
    
    // 4. Send via E1000
    // e1000_send_packet(...);
    
    return ERR_OK;
}
