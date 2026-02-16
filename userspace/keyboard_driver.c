#include <stdint.h>
#include "syscalls.h"
#include "u_stdlib.h"

// I/O Ports
#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

// Commands
#define KBD_CMD_ENABLE  0xAE

// Status bits
#define STATUS_OUTPUT_FULL 0x01

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    syscall3(SYS_WRITE, 1, (long)"[KBD] Keyboard Driver Started\n", 30);
    
    // 1. Create IPC Port
    long port_id = syscall1(SYS_IPC_CREATE, 0); // 0 = Default flags
    if (port_id <= 0) {
        syscall3(SYS_WRITE, 1, (long)"[KBD] Failed to create IPC port\n", 32);
        syscall1(SYS_EXIT, 1);
    }

    // 2. Register IPC Port "keyboard"
    long res = syscall2(SYS_IPC_REGISTER, port_id, (long)"keyboard");
    if (res < 0) {
        syscall3(SYS_WRITE, 1, (long)"[KBD] Failed to register IPC port\n", 34);
        syscall1(SYS_EXIT, 1);
    }
    
    // 2. Enable Keyboard (if needed)
    // syscall3(SYS_IOPORT, KBD_CMD_ENABLE, KBD_STATUS_PORT, 1); // Write

    long comp_port = 0;
    
    while (1) {
        // Wait for IRQ 1
        syscall1(SYS_IRQ_WAIT, 1);
        
        // Poll status port
        uint8_t status = (uint8_t)syscall3(SYS_IOPORT, KBD_STATUS_PORT, 0, 0);
        
        while (status & STATUS_OUTPUT_FULL) {
            uint8_t scancode = (uint8_t)syscall3(SYS_IOPORT, KBD_DATA_PORT, 0, 0);
            
            // Lookup compositor if not found yet
            if (comp_port <= 0) comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
            
            if (comp_port > 0) {
                struct {
                    uint32_t type;
                    uint32_t code;
                    int32_t x;
                    int32_t y;
                } msg;
                msg.type = 1; // EVENT_KEY_DOWN
                msg.code = scancode;
                msg.x = 0;
                msg.y = 0;
                
                syscall3(SYS_IPC_SEND, comp_port, (long)&msg, sizeof(msg));
            }
            
            status = (uint8_t)syscall3(SYS_IOPORT, KBD_STATUS_PORT, 0, 0);
        }
    }
}
