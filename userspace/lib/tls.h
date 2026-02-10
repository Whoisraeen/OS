#ifndef TLS_H
#define TLS_H

#include <stdint.h>
#include <stddef.h>

// TLS Context
typedef struct tls_ctx tls_ctx_t;

// API
tls_ctx_t *tls_create_context(void);
void tls_destroy_context(tls_ctx_t *ctx);

int tls_connect(tls_ctx_t *ctx, int sockfd, const char *hostname);
int tls_write(tls_ctx_t *ctx, const void *data, size_t len);
int tls_read(tls_ctx_t *ctx, void *data, size_t len);
int tls_close(tls_ctx_t *ctx);

#endif
