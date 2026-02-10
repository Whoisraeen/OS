#include "lwip/tcp.h"
#include "lwip/endian.h"
#include "heap.h"
#include "console.h"
#include "serial.h"
#include "string.h"

struct tcp_pcb *tcp_active_pcbs = NULL;
struct tcp_pcb *tcp_listen_pcbs = NULL;

struct tcp_pcb *tcp_new(void) {
    struct tcp_pcb *pcb = kmalloc(sizeof(struct tcp_pcb));
    if (pcb) {
        memset(pcb, 0, sizeof(struct tcp_pcb));
        pcb->state = CLOSED;
    }
    return pcb;
}

void tcp_bind(struct tcp_pcb *pcb, uint32_t ip, uint16_t port) {
    if (pcb) {
        pcb->local_ip = ip;
        pcb->local_port = port;
    }
}

struct tcp_pcb *tcp_listen(struct tcp_pcb *pcb) {
    if (pcb) {
        pcb->state = LISTEN;
        pcb->next = tcp_listen_pcbs;
        tcp_listen_pcbs = pcb;
    }
    return pcb;
}

void tcp_connect(struct tcp_pcb *pcb, uint32_t ip, uint16_t port, err_t (*connected)(void *arg, struct tcp_pcb *pcb, err_t err)) {
    if (pcb) {
        pcb->remote_ip = ip;
        pcb->remote_port = port;
        pcb->state = SYN_SENT;
        
        // Mock local port
        if (pcb->local_port == 0) pcb->local_port = 49152; 

        // Send SYN (mock)
        kprintf("[TCP] Sending SYN to %08x:%d\n", ip, port);
        
        // Add to active list
        pcb->next = tcp_active_pcbs;
        tcp_active_pcbs = pcb;
        
        // Mock connection success for Gemini client test
        if (connected) connected(pcb->callback_arg, pcb, ERR_OK);
    }
}

void tcp_write(struct tcp_pcb *pcb, const void *dataptr, uint16_t len, uint8_t apiflags) {
    (void)apiflags;
    if (pcb) {
        kprintf("[TCP] Sending %d bytes\n", len);
        // ip_output(...)
    }
}

void tcp_close(struct tcp_pcb *pcb) {
    // Remove from active list
    if (tcp_active_pcbs == pcb) {
        tcp_active_pcbs = pcb->next;
    } else {
        struct tcp_pcb *prev = tcp_active_pcbs;
        while (prev && prev->next != pcb) prev = prev->next;
        if (prev) prev->next = pcb->next;
    }
    kfree(pcb);
}

void tcp_input(struct pbuf *p, struct netif *inp) {
    (void)inp;
    if (p->len < sizeof(struct tcp_hdr)) {
        pbuf_free(p);
        return;
    }
    
    struct tcp_hdr *tcphdr = (struct tcp_hdr *)p->payload;
    uint16_t src_port = ntohs(tcphdr->src);
    uint16_t dest_port = ntohs(tcphdr->dest);
    
    // Find matching PCB
    struct tcp_pcb *pcb = tcp_active_pcbs;
    while (pcb != NULL) {
        if (pcb->local_port == dest_port && pcb->remote_port == src_port) {
            break;
        }
        pcb = pcb->next;
    }
    
    if (pcb && pcb->recv) {
        // Adjust pbuf to payload
        uint16_t hlen = TCPH_HDRLEN(tcphdr) * 4;
        p->payload = (uint8_t*)p->payload + hlen;
        p->len -= hlen;
        
        pcb->recv(pcb->callback_arg, pcb, p, ERR_OK);
    } else {
        pbuf_free(p);
    }
}