#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"
#include "u_stdlib.h"

// Utils moved to u_stdlib.h

static int _getpid(void) {
    return syscall0(SYS_GETPID);
}

// Define printf wrapper if u_stdlib doesn't have it exposed or include stdio.h if available
#define printf(...) do { char buf[128]; snprintf(buf, 128, __VA_ARGS__); syscall3(SYS_WRITE, 1, (long)buf, strlen(buf)); } while(0)

// Audio Server
// Manages audio mixing and output to the kernel HDA driver via IPC or shared memory
// For now, it's a stub that registers itself as the "audio" service.

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // main(0, NULL);
    // Inline main logic for _start
    
    // Register as audio service
    // In a real implementation, we would open /dev/dsp or similar (implemented by HDA driver)
    // and mix multiple audio streams from other processes.
    
    printf("Audio Server Started (PID %d)\n", _getpid());
    
    // Create an IPC port for audio requests
    int port = syscall1(SYS_IPC_CREATE, IPC_PORT_FLAG_RECEIVE);
    if (port < 0) {
        printf("Failed to create audio port\n");
        syscall1(SYS_EXIT, 1);
    }
    
    // Register the port
    syscall2(SYS_IPC_REGISTER, (long)"system.audio", port);
    
    printf("Listening on port %d\n", port);
    
    while (1) {
        // Wait for messages
        // ipc_message_t msg;
        // int sender = syscall3(SYS_RECV_MESSAGE, port, &msg, sizeof(msg));
        
        // Handle play request, set volume, etc.
        
        syscall0(SYS_SCHED_YIELD);
    }
}
