#include "lwip/ip.h"
#include "lwip/netif.h"
#include "console.h"

void ip_input(struct pbuf *p, struct netif *inp) {
    (void)inp;
    if (p->len < sizeof(struct ip_hdr)) {
        pbuf_free(p);
        return;
    }
    
    struct ip_hdr *ip = (struct ip_hdr *)p->payload;
    // Basic checks
    if (IP_V(ip) != 4) {
        pbuf_free(p);
        return;
    }
    
    kprintf("[IP] Received packet from %08x to %08x\n", ip->src, ip->dest);
    
    // Check if it's for us (simple check)
    // if (ip->dest == netif_ip_addr(inp)) ...
    
    // Dispatch to UDP/TCP (Not implemented yet)
    
    pbuf_free(p);
}

err_t ip_output(struct pbuf *p, u32_t src, u32_t dest, u8_t ttl, u8_t proto) {
    // Prepend IP header
    // Use pbuf_header(p, IP_HLEN)
    // Fill header
    // Checksum
    // Send to netif
    (void)p; (void)src; (void)dest; (void)ttl; (void)proto;
    return ERR_OK;
}
