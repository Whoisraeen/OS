#define SSL_COMPATIBLE_INTERFACE
#define TLS_AMALGAMATION
#include "tlse/tlse.h"
#include "tls.h"
#include "../syscalls.h"
#include "../u_stdlib.h"

struct tls_ctx {
    SSL *ssl;
    SSL_CTX *ctx;
};

// Syscall-based I/O for TLSe
static int tls_low_recv(int fd, void *buffer, size_t length, int flags) {
    (void)flags;
    return (int)syscall3(SYS_RECV, fd, (long)buffer, length);
}

static int tls_low_send(int fd, const void *buffer, size_t length, int flags) {
    (void)flags;
    return (int)syscall3(SYS_SEND, fd, (long)buffer, length);
}

tls_ctx_t *tls_create_context(void) {
    tls_ctx_t *t = malloc(sizeof(tls_ctx_t));
    if (!t) return NULL;

    SSL_library_init();
    t->ctx = SSL_CTX_new(SSLv3_client_method());
    if (!t->ctx) {
        free(t);
        return NULL;
    }
    
    t->ssl = SSL_new(t->ctx);
    if (!t->ssl) {
        SSL_CTX_free(t->ctx);
        free(t);
        return NULL;
    }

    SSL_set_io(t->ssl, tls_low_recv, tls_low_send);
    return t;
}

void tls_destroy_context(tls_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->ssl) SSL_free(ctx->ssl);
    if (ctx->ctx) SSL_CTX_free(ctx->ctx);
    free(ctx);
}

int tls_connect(tls_ctx_t *ctx, int sockfd, const char *hostname) {
    if (!ctx || !ctx->ssl) return -1;
    
    SSL_set_fd(ctx->ssl, sockfd);
    // TODO: Set SNI if needed via TLSe native API since SSL_compat might not have it
    
    return SSL_connect(ctx->ssl) == 1 ? 0 : -1;
}

int tls_write(tls_ctx_t *ctx, const void *data, size_t len) {
    if (!ctx || !ctx->ssl) return -1;
    return SSL_write(ctx->ssl, data, len);
}

int tls_read(tls_ctx_t *ctx, void *data, size_t len) {
    if (!ctx || !ctx->ssl) return -1;
    return SSL_read(ctx->ssl, data, len);
}

int tls_close(tls_ctx_t *ctx) {
    if (!ctx || !ctx->ssl) return -1;
    return SSL_shutdown(ctx->ssl);
}