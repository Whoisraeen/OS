#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"
#include "u_stdlib.h"
#include "lib/tls.h"

// Gemini Client for RaeenOS
// Uses TLSe for mandatory encryption

#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
} sockaddr_in_t;

// Helper to convert IP string to dword (simplified)
static uint32_t inet_addr(const char *cp) {
    uint32_t addr = 0;
    uint32_t part = 0;
    int shift = 0;
    while (*cp) {
        if (*cp == '.') {
            addr |= (part << shift);
            part = 0;
            shift += 8;
        } else {
            part = part * 10 + (*cp - '0');
        }
        cp++;
    }
    addr |= (part << shift);
    return addr;
}

void _start(void) {
    const char *url = "gemini://raeenos.org/";
    const char *hostname = "raeenos.org";
    const char *ip_str = "127.0.0.1"; // Default gateway/local test
    
    printf("RaeenOS Gemini CLI v1.0\n");
    printf("Fetching: %s\n", url);
    
    // 1. Create Socket
    int sockfd = (int)syscall3(SYS_SOCKET, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        printf("Error: Socket creation failed\n");
        syscall1(SYS_EXIT, 1);
    }
    
    // 2. Connect
    sockaddr_in_t addr;
    addr.sin_family = AF_INET;
    addr.sin_port = 1965; 
    addr.sin_addr = inet_addr(ip_str);
    
    printf("Connecting to %s:%d...\n", ip_str, addr.sin_port);
    int ret = (int)syscall3(SYS_CONNECT, sockfd, (long)&addr, sizeof(addr));
    
    if (ret < 0) {
        printf("Error: Connection failed\n");
        syscall1(SYS_EXIT, 1);
    }
    
    printf("Connected. Performing TLS handshake...\n");
    
    // 3. TLS Handshake
    tls_ctx_t *tls = tls_create_context();
    if (!tls) {
        printf("Error: TLS context allocation failed\n");
        syscall1(SYS_EXIT, 1);
    }
    
    if (tls_connect(tls, sockfd, hostname) < 0) {
        printf("Error: TLS handshake failed\n");
        tls_destroy_context(tls);
        syscall1(SYS_EXIT, 1);
    }
    
    printf("TLS established. Sending request...\n");
    
    // 4. Send Gemini Request
    // Format: <URL>\r\n
    char request[512];
    int req_len = snprintf(request, sizeof(request), "%s\r\n", url);
    tls_write(tls, request, req_len);
    
    // 5. Read Response
    printf("--- Response ---\n");
    char buffer[2048];
    int bytes;
    while ((bytes = tls_read(tls, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes] = '\0';
        printf("%s", buffer);
    }
    printf("\n----------------\n");
    
    // 6. Cleanup
    tls_close(tls);
    tls_destroy_context(tls);
    syscall1(SYS_CLOSE, sockfd);
    
    printf("Done.\n");
    syscall1(SYS_EXIT, 0);
}