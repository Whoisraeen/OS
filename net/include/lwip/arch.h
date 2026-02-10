#ifndef LWIP_ARCH_H
#define LWIP_ARCH_H

#include <stdint.h>
#include "string.h"

#define u8_t uint8_t
#define s8_t int8_t
#define u16_t uint16_t
#define s16_t int16_t
#define u32_t uint32_t
#define s32_t int32_t
#define mem_ptr_t uintptr_t

#define LWIP_UNUSED_ARG(x) (void)x
#define LWIP_PLATFORM_DIAG(x) kprintf x
#define LWIP_PLATFORM_ASSERT(x) do { kprintf("Assertion \"%s\" failed at line %d in %s\n", x, __LINE__, __FILE__); } while(0)

#define BYTE_ORDER LITTLE_ENDIAN

// Packet Buffer Types
#define PBUF_RAW 0
#define PBUF_RAM 1
#define PBUF_ROM 2
#define PBUF_REF 3
#define PBUF_POOL 4

// Error codes
typedef int8_t err_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_BUF -2
#define ERR_TIMEOUT -3
#define ERR_RTE -4
#define ERR_INPROGRESS -5
#define ERR_VAL -6
#define ERR_WOULDBLOCK -7
#define ERR_USE -8
#define ERR_ALREADY -9
#define ERR_ISCONN -10
#define ERR_CONN -11
#define ERR_IF -12
#define ERR_ABRT -13
#define ERR_RST -14
#define ERR_CLSD -15
#define ERR_ARG -16

#ifndef NULL
#define NULL 0
#endif

#endif
