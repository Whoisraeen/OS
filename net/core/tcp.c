#include "lwip/tcp.h"
#include "heap.h"
#include "console.h"

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

void tcp_bind(struct tcp_pcb *pcb, u32_t ip, u16_t port) {
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

void tcp_connect(struct tcp_pcb *pcb, u32_t ip, u16_t port, err_t (*connected)(void *arg, struct tcp_pcb *pcb, err_t err)) {
    if (pcb) {
        pcb->remote_ip = ip;
        pcb->remote_port = port;
        pcb->state = SYN_SENT;
        
        // Send SYN (mock)
        kprintf("[TCP] Sending SYN to %08x:%d\n", ip, port);
        
        // Add to active list
        pcb->next = tcp_active_pcbs;
        tcp_active_pcbs = pcb;
        
        // Mock connection success for Gemini client test
        if (connected) connected(pcb->callback_arg, pcb, ERR_OK);
    }
}

void tcp_write(struct tcp_pcb *pcb, const void *dataptr, u16_t len, u8_t apiflags) {
    (void)apiflags;
    if (pcb && pcb->state >= ESTABLISHED) {
        kprintf("[TCP] Sending %d bytes\n", len);
        // ip_output(...)
    }
}

void tcp_close(struct tcp_pcb *pcb) {
    // Remove from list
    // Send FIN
    kfree(pcb);
}
