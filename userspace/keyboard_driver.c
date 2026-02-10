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

void _start(void) {
    syscall3(SYS_WRITE, 1, (long)"[KBD] Keyboard Driver Started\n", 30);
    
    // 1. Register IPC Port "keyboard"
    long port_id = syscall2(SYS_IPC_REGISTER, 0, (long)"keyboard");
    if (port_id < 0) {
        syscall3(SYS_WRITE, 1, (long)"[KBD] Failed to register IPC port\n", 34);
        syscall1(SYS_EXIT, 1);
    }
    
    // 2. Enable Keyboard (if needed)
    // syscall3(SYS_IOPORT, KBD_CMD_ENABLE, KBD_STATUS_PORT, 1); // Write

    while (1) {
        // Poll status port
        uint8_t status = (uint8_t)syscall3(SYS_IOPORT, KBD_STATUS_PORT, 0, 0);
        
        if (status & STATUS_OUTPUT_FULL) {
            uint8_t scancode = (uint8_t)syscall3(SYS_IOPORT, KBD_DATA_PORT, 0, 0);
            
            // Broadcast scancode via IPC
            // For now, just print it to debug
            // char buf[32];
            // int len = snprintf(buf, 32, "[KBD] Scancode: %x\n", scancode);
            // syscall3(SYS_WRITE, 1, (long)buf, len);
            
            // Send to Compositor (assuming it listens on "compositor_input")
            // For simplicity, we just send to anyone listening on our port
            // But usually drivers PUSH events to the window server.
            
            long comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
            if (comp_port > 0) {
                // Send compatible msg_input_event_t
                struct {
                    uint32_t type;
                    uint32_t code;
                    int32_t x;
                    int32_t y;
                } msg;
                msg.type = 1; // EVENT_KEY_DOWN (matches gui.h EVENT_KEY_DOWN=1)
                msg.code = scancode;
                msg.x = 0;
                msg.y = 0;
                
                syscall3(SYS_IPC_SEND, comp_port, (long)&msg, sizeof(msg));
            }
        }
        
        // Yield to avoid 100% CPU usage (since we are polling for now)
        syscall3(SYS_YIELD, 0, 0, 0);
        
        // Ideally: syscall1(SYS_IRQ_WAIT, 1);
    }
}
