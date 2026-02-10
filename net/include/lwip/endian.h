#ifndef LWIP_ENDIAN_H
#define LWIP_ENDIAN_H

#include <stdint.h>

static inline uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort & 0xFF00) >> 8);
}

static inline uint16_t ntohs(uint16_t netshort) {
    return htons(netshort);
}

static inline uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) |
           ((hostlong & 0xFF00) << 8) |
           ((hostlong & 0xFF0000) >> 8) |
           ((hostlong & 0xFF000000) >> 24);
}

static inline uint32_t ntohl(uint32_t netlong) {
    return htonl(netlong);
}

#endif
