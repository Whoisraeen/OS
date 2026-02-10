#include "tls.h"
#include <stddef.h>
#include <stdint.h>
#include "../u_stdlib.h"
#include "../syscalls.h"

// Stub TLS implementation
// For now, this just passes data through to the socket (unencrypted)
// This allows the Gemini client to "work" (if server accepts plaintext)
// or at least verify the pipeline.
// In future, link mbedTLS here.

struct tls_ctx {
    int sockfd;
};

tls_ctx_t *tls_create_context(void) {
    tls_ctx_t *ctx = malloc(sizeof(tls_ctx_t));
    if (ctx) {
        ctx->sockfd = -1;
    }
    return ctx;
}

void tls_destroy_context(tls_ctx_t *ctx) {
    free(ctx);
}

int tls_connect(tls_ctx_t *ctx, int sockfd, const char *hostname) {
    if (!ctx) return -1;
    ctx->sockfd = sockfd;
    
    // Perform Handshake (Stub)
    // 1. Send ClientHello
    // 2. Recv ServerHello
    // ...
    (void)hostname;
    
    // For now, just assume success
    return 0;
}

int tls_write(tls_ctx_t *ctx, const void *data, size_t len) {
    if (!ctx || ctx->sockfd < 0) return -1;
    
    // Encrypt data (Stub: just send raw)
    return syscall3(SYS_SEND, ctx->sockfd, (long)data, len);
}

int tls_read(tls_ctx_t *ctx, void *data, size_t len) {
    if (!ctx || ctx->sockfd < 0) return -1;
    
    // Decrypt data (Stub: just recv raw)
    return syscall3(SYS_RECV, ctx->sockfd, (long)data, len);
}

int tls_close(tls_ctx_t *ctx) {
    if (!ctx) return -1;
    // Send close notify
    ctx->sockfd = -1;
    return 0;
}
