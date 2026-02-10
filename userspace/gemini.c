#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"
#include "u_stdlib.h"

static size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

#define printf(...) do { char buf[128]; snprintf(buf, 128, __VA_ARGS__); syscall3(SYS_WRITE, 1, (long)buf, strlen(buf)); } while(0)

// Simple Gemini Client (Skeleton)
// Protocol: gemini://<host>[:port]/<path>
// Default port: 1965
// TLS is mandatory (Mocked for now)

// Mock socket defines
#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
} sockaddr_in;

void _start(void) {
    // Hardcoded URL for test since no argv parsing yet in _start
    char *url = "gemini://raeenos.org";
    printf("Fetching %s...\n", url);
    
    // Parse URL (Simplified)
    // Assume gemini://host/path format
    // For now, we just hardcode connecting to a dummy IP
    
    // 1. Create Socket
    int sockfd = syscall3(SYS_SOCKET, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        printf("Error: Failed to create socket (Syscall not fully implemented?)\n");
        // return; // Continue for demo purposes
    }
    
    // 2. Connect
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = 1965; // Big endian?
    addr.sin_addr = 0x0100007F; // 127.0.0.1
    
    int ret = syscall3(SYS_CONNECT, sockfd, (long)&addr, sizeof(addr));
    if (ret < 0) {
        printf("Error: Connection failed (Networking stack not ready)\n");
    } else {
        printf("Connected!\n");
        
        // 3. Send Request
        // <URL><CR><LF>
        char req[256];
        int len = 0;
        // strcpy(req, url); ...
        req[0] = '\r'; req[1] = '\n'; req[2] = 0;
        
        syscall3(SYS_SEND, sockfd, (long)req, 2);
        
        // 4. Receive Response
        char buf[1024];
        int n = syscall3(SYS_RECV, sockfd, (long)buf, 1024);
        if (n > 0) {
            buf[n] = 0;
            printf("%s\n", buf);
        }
    }
    
    // Fallback Demo Output
    printf("\n[DEMO] Gemini Response:\n");
    printf("20 text/gemini\r\n");
    printf("# Welcome to RaeenOS Gemini Capsule\n");
    printf("This is a placeholder response until the TCP/IP stack is fully online.\n");
    printf("\n");
    printf("=> gemini://raeenos.org Project Homepage\n");
    
    syscall1(SYS_EXIT, 0);
}
