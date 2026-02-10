#ifndef LWIP_NETIF_H
#define LWIP_NETIF_H

#include "arch.h"
#include "pbuf.h"

struct netif;

typedef err_t (*netif_input_fn)(struct pbuf *p, struct netif *inp);
typedef err_t (*netif_linkoutput_fn)(struct netif *netif, struct pbuf *p);

struct netif {
    struct netif *next;
    u8_t hwaddr_len;
    u8_t hwaddr[6];
    u16_t mtu;
    u8_t flags;
    char name[2];
    u8_t num;
    void *state;
    netif_input_fn input;
    netif_linkoutput_fn linkoutput;
};

struct netif *netif_add(struct netif *netif, void *state, netif_input_fn input, netif_linkoutput_fn linkoutput);
void netif_set_default(struct netif *netif);
void netif_set_up(struct netif *netif);

#endif
