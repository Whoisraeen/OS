#include "lwip/pbuf.h"
#include "heap.h"
#include "string.h"

struct pbuf *pbuf_alloc(int layer, u16_t length, int type) {
    (void)layer;
    struct pbuf *p = kmalloc(sizeof(struct pbuf));
    if (!p) return NULL;
    
    if (type == PBUF_POOL || type == PBUF_RAM) {
        p->payload = kmalloc(length);
        if (!p->payload) {
            kfree(p);
            return NULL;
        }
    } else {
        p->payload = NULL;
    }
    
    p->next = NULL;
    p->len = length;
    p->tot_len = length;
    p->type = type;
    p->ref = 1;
    
    return p;
}

void pbuf_free(struct pbuf *p) {
    struct pbuf *q;
    while (p != NULL) {
        if (--p->ref == 0) {
            q = p->next;
            if (p->type == PBUF_POOL || p->type == PBUF_RAM) {
                if (p->payload) kfree(p->payload);
            }
            kfree(p);
            p = q;
        } else {
            break;
        }
    }
}

void pbuf_chain(struct pbuf *h, struct pbuf *t) {
    if (h == NULL || t == NULL) return;
    struct pbuf *p = h;
    while (p->next != NULL) p = p->next;
    p->next = t;
    h->tot_len += t->tot_len;
}

u8_t pbuf_get_at(const struct pbuf* p, u16_t offset) {
    while (p != NULL) {
        if (offset < p->len) {
            return ((u8_t*)p->payload)[offset];
        }
        offset -= p->len;
        p = p->next;
    }
    return 0;
}
