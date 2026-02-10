#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <stdint.h>
// Use endian helpers from lwip if possible, or just mock here
static inline uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort & 0xFF00) >> 8);
}
#endif
