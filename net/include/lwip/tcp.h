#ifndef LWIP_TCP_H
#define LWIP_TCP_H

#include "arch.h"
#include "ip.h"

struct tcp_pcb {
    struct tcp_pcb *next;
    enum {
        CLOSED,
        LISTEN,
        SYN_SENT,
        SYN_RCVD,
        ESTABLISHED,
        FIN_WAIT_1,
        FIN_WAIT_2,
        CLOSE_WAIT,
        CLOSING,
        LAST_ACK,
        TIME_WAIT
    } state;
    
    u32_t local_ip;
    u16_t local_port;
    u32_t remote_ip;
    u16_t remote_port;
    
    // Callbacks
    err_t (*recv)(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
    void *callback_arg;
};

struct tcp_hdr {
    u16_t src;
    u16_t dest;
    u32_t seqno;
    u32_t ackno;
    u16_t _hdrlen_flags;
    u16_t wnd;
    u16_t chksum;
    u16_t urgp;
} __attribute__((packed));

#define TCPH_HDRLEN(phdr) (ntohs((phdr)->_hdrlen_flags) >> 12)
#define TCPH_FLAGS(phdr)  (ntohs((phdr)->_hdrlen_flags) & 0x3f)

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

struct tcp_pcb *tcp_new(void);
void tcp_bind(struct tcp_pcb *pcb, u32_t ip, u16_t port);
struct tcp_pcb *tcp_listen(struct tcp_pcb *pcb);
void tcp_connect(struct tcp_pcb *pcb, u32_t ip, u16_t port, err_t (*connected)(void *arg, struct tcp_pcb *pcb, err_t err));
void tcp_write(struct tcp_pcb *pcb, const void *dataptr, u16_t len, u8_t apiflags);
void tcp_close(struct tcp_pcb *pcb);
void tcp_input(struct pbuf *p, struct netif *inp);

#endif
