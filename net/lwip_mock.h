#ifndef LWIP_MOCK_H
#define LWIP_MOCK_H

#include <stdint.h>

// Mock definitions for lwIP structures to allow compilation of the adapter layer
// without the actual lwIP source tree.

// Error codes
typedef int8_t err_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_BUF -2
#define ERR_TIMEOUT -3
#define ERR_RTE -4

// PBUF (Packet Buffer)
#define PBUF_RAW 0
#define PBUF_RAM 1
#define PBUF_ROM 2
#define PBUF_REF 3
#define PBUF_POOL 4

typedef struct pbuf {
    struct pbuf *next;
    void *payload;
    uint16_t tot_len;
    uint16_t len;
    uint8_t type;
    uint8_t flags;
    uint16_t ref;
} pbuf_t;

// NETIF (Network Interface)
typedef struct netif {
    struct netif *next;
    uint8_t hwaddr_len;
    uint8_t hwaddr[6];
    uint16_t mtu;
    uint8_t flags;
    char name[2];
    uint8_t num;
    void *state;
    // Function pointers would go here in real lwIP
    err_t (*input)(struct pbuf *p, struct netif *inp);
    err_t (*linkoutput)(struct netif *netif, struct pbuf *p);
} netif_t;

// Prototypes for functions we'd expect lwIP to provide
struct pbuf *pbuf_alloc(int layer, uint16_t length, int type);
void pbuf_free(struct pbuf *p);

#endif
