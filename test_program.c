#include <stdint.h>

#define SYS_EXIT         0
#define SYS_WRITE        1
#define SYS_READ         3
#define SYS_YIELD        24
#define SYS_IPC_CREATE   10
#define SYS_IPC_SEND     11
#define SYS_IPC_RECV     12

// IPC Flags
#define IPC_PORT_FLAG_NONE    0
#define IPC_PORT_FLAG_RECEIVE (1 << 1)
#define IPC_PORT_FLAG_SEND    (1 << 2)

// IPC Message Structure (must match kernel)
typedef struct {
    uint32_t msg_id;
    uint32_t sender_pid;
    uint32_t reply_port;
    uint32_t size;
    uint64_t timestamp;
    uint8_t data[128];
} ipc_message_t;

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall1(long n, long a1) {
    return syscall3(n, a1, 0, 0);
}

void _start(void) {
    // 1. Create a port to receive messages
    long port = syscall1(SYS_IPC_CREATE, IPC_PORT_FLAG_RECEIVE);
    
    if (port <= 0) {
        syscall3(SYS_WRITE, 1, (long)"Failed to create port\n", 22);
        syscall3(SYS_EXIT, 1, 0, 0);
    }

    // 2. Send a message to ourselves
    ipc_message_t msg;
    msg.size = 12;
    // Copy "Hello IPC!" to data
    char *txt = "Hello IPC!";
    for (int i=0; i<11; i++) msg.data[i] = txt[i];
    msg.data[11] = 0;
    
    // SYS_IPC_SEND(dest_port, msg_ptr, flags)
    long res = syscall3(SYS_IPC_SEND, port, (long)&msg, 0);
    
    if (res == 0) {
         syscall3(SYS_WRITE, 1, (long)"Message sent!\n", 14);
    } else {
         syscall3(SYS_WRITE, 1, (long)"Send failed\n", 12);
    }

    // 3. Receive the message
    ipc_message_t recv_msg;
    // SYS_IPC_RECV(port, msg_ptr, flags)
    res = syscall3(SYS_IPC_RECV, port, (long)&recv_msg, 0);
    
    if (res == 0) {
        syscall3(SYS_WRITE, 1, (long)"Message received: ", 18);
        syscall3(SYS_WRITE, 1, (long)recv_msg.data, recv_msg.size);
        syscall3(SYS_WRITE, 1, (long)"\n", 1);
    } else {
        syscall3(SYS_WRITE, 1, (long)"Recv failed\n", 12);
    }

    syscall3(SYS_EXIT, 0, 0, 0);
    for(;;) {}
}
