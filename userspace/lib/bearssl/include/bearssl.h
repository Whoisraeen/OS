#ifndef BEARSSL_H
#define BEARSSL_H

#include <stddef.h>
#include <stdint.h>

/* BearSSL core structures (Minimal for Port) */

typedef struct {
    uint32_t (*read)(void *ctx, unsigned char *buf, size_t len);
    uint32_t (*write)(void *ctx, const unsigned char *buf, size_t len);
} br_ssl_io_class;

typedef struct {
    const br_ssl_io_class *vtable;
    int sockfd;
} br_sslio_context;

/* SSL Engine Context */
typedef struct {
    unsigned char *ibuf;
    size_t ibuf_len;
    unsigned char *obuf;
    size_t obuf_len;
    // ... Internal State ...
} br_ssl_engine_context;

typedef struct {
    br_ssl_engine_context eng;
    // ... Client Specific ...
} br_ssl_client_context;

/* Simplified X.509 */
typedef struct {
    // ...
} br_x509_minimal_context;

/* Function Prototypes */
void br_ssl_client_init_full(br_ssl_client_context *cc, br_x509_minimal_context *xc, const void *trust_anchors, size_t trust_anchors_num);
void br_sslio_init(br_sslio_context *io, br_ssl_engine_context *eng, int (*low_read)(void *, unsigned char *, size_t), void *read_ctx, int (*low_write)(void *, const unsigned char *, size_t), void *write_ctx);

#endif
