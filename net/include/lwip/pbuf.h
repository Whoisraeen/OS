#ifndef LWIP_PBUF_H
#define LWIP_PBUF_H

#include "arch.h"

struct pbuf {
    struct pbuf *next;
    void *payload;
    u16_t tot_len;
    u16_t len;
    u8_t type;
    u8_t flags;
    u16_t ref;
};

struct pbuf *pbuf_alloc(int layer, u16_t length, int type);
void pbuf_free(struct pbuf *p);
void pbuf_chain(struct pbuf *h, struct pbuf *t);
u8_t pbuf_get_at(const struct pbuf* p, u16_t offset);

#endif
