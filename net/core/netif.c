#include "lwip/netif.h"
#include "stddef.h"

struct netif *netif_list = NULL;
struct netif *netif_default = NULL;

struct netif *netif_add(struct netif *netif, void *state, netif_input_fn input, netif_linkoutput_fn linkoutput) {
    netif->state = state;
    netif->input = input;
    netif->linkoutput = linkoutput;
    netif->next = netif_list;
    netif_list = netif;
    return netif;
}

void netif_set_default(struct netif *netif) {
    netif_default = netif;
}

void netif_set_up(struct netif *netif) {
    if (netif) {
        netif->flags |= 1; // UP
    }
}
