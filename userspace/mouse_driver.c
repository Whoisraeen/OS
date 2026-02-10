#include <stdint.h>
#include "syscalls.h"
#include "u_stdlib.h"

// I/O Ports
#define MOUSE_DATA_PORT   0x60
#define MOUSE_STATUS_PORT 0x64

// Commands
#define MOUSE_CMD_ENABLE  0xA8
#define MOUSE_CMD_WRITE   0xD4

// Status bits
#define STATUS_OUTPUT_FULL 0x01
#define STATUS_MOUSE_DATA  0x20

// State
static int mouse_x = 100;
static int mouse_y = 100;
static uint8_t mouse_cycle = 0;
static int8_t mouse_bytes[3];
static uint8_t last_buttons = 0;

// Wait for mouse controller
static void mouse_wait(int type) {
    int timeout = 100000;
    if (type == 0) {
        // Wait for input buffer to be clear
        while (timeout--) {
            uint8_t status = syscall3(SYS_IOPORT, MOUSE_STATUS_PORT, 0, 0);
            if (!(status & 2)) break;
        }
    } else {
        // Wait for output buffer to be full
        while (timeout--) {
            uint8_t status = syscall3(SYS_IOPORT, MOUSE_STATUS_PORT, 0, 0);
            if (status & 1) break;
        }
    }
}

static void mouse_write(uint8_t data) {
    mouse_wait(0);
    syscall3(SYS_IOPORT, MOUSE_STATUS_PORT, MOUSE_CMD_WRITE, 1);
    mouse_wait(0);
    syscall3(SYS_IOPORT, MOUSE_DATA_PORT, data, 1);
}

static uint8_t mouse_read(void) {
    mouse_wait(1);
    return syscall3(SYS_IOPORT, MOUSE_DATA_PORT, 0, 0);
}

void _start(void) {
    syscall3(SYS_WRITE, 1, (long)"[MOUSE] Driver Started\n", 23);

    // 1. Register IPC Port
    long port_id = syscall2(SYS_IPC_REGISTER, 0, (long)"mouse");
    if (port_id < 0) {
        syscall3(SYS_WRITE, 1, (long)"[MOUSE] Failed to register port\n", 32);
        syscall1(SYS_EXIT, 1);
    }

    // 2. Initialize Mouse (Simple Sequence)
    // Enable Aux Device
    mouse_wait(0);
    syscall3(SYS_IOPORT, MOUSE_STATUS_PORT, MOUSE_CMD_ENABLE, 1);
    
    // Enable Interrupts (needed for controller to buffer data even if we poll?)
    // Actually, we need to ensure the controller knows we want mouse data.
    // The kernel mouse_init already did the heavy lifting. 
    // We just need to enable data reporting again just in case.
    mouse_write(0xF4);
    mouse_read(); // ACK

    long comp_port = 0;

    while (1) {
        // Find compositor if not found yet
        if (comp_port <= 0) {
            comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
        }

        // Poll status
        uint8_t status = syscall3(SYS_IOPORT, MOUSE_STATUS_PORT, 0, 0);
        
        if ((status & STATUS_OUTPUT_FULL) && (status & STATUS_MOUSE_DATA)) {
            uint8_t data = syscall3(SYS_IOPORT, MOUSE_DATA_PORT, 0, 0);
            
            switch (mouse_cycle) {
                case 0:
                    mouse_bytes[0] = data;
                    if (data & 0x08) mouse_cycle++;
                    break;
                case 1:
                    mouse_bytes[1] = data;
                    mouse_cycle++;
                    break;
                case 2:
                    mouse_bytes[2] = data;
                    mouse_cycle = 0;
                    
                    // Decode
                    uint8_t state = mouse_bytes[0];
                    int dx = (int)mouse_bytes[1];
                    int dy = (int)mouse_bytes[2];
                    
                    if (state & 0x10) dx |= 0xFFFFFF00;
                    if (state & 0x20) dy |= 0xFFFFFF00;
                    
                    // Update relative position (Compositor handles absolute)
                    // Wait, compositor expects absolute?
                    // Let's look at compositor.c ... it tracks its own mouse_x/y
                    // but it expects `evt.x` and `evt.y` from the driver.
                    // The keyboard driver sends 0,0.
                    // The compositor updates `mouse_x` from `evt.x`.
                    // So we MUST send absolute coordinates.
                    
                    mouse_x += dx;
                    mouse_y -= dy; // Invert Y
                    
                    // Clamp (Arbitrary bounds, compositor will clamp to screen)
                    if (mouse_x < 0) mouse_x = 0;
                    if (mouse_x > 1024) mouse_x = 1024;
                    if (mouse_y < 0) mouse_y = 0;
                    if (mouse_y > 768) mouse_y = 768;
                    
                    uint8_t buttons = state & 0x07;
                    
                    // Send to Compositor
                    if (comp_port > 0) {
                        struct {
                            uint32_t type;
                            uint32_t code;
                            int32_t x;
                            int32_t y;
                        } msg;
                        msg.type = 2; // EVENT_MOUSE_MOVE (custom? Gui.h has 3 for move, 2 is KeyUp?)
                        // gui.h:
                        // EVENT_KEY_DOWN 1
                        // EVENT_KEY_UP 2  <-- Wait, let's check gui.h again
                        // EVENT_MOUSE_MOVE 3
                        // EVENT_MOUSE_DOWN 4
                        // EVENT_MOUSE_UP 5
                        
                        // Let's send MOVE first
                        msg.type = 3; // EVENT_MOUSE_MOVE
                        msg.code = buttons;
                        msg.x = mouse_x;
                        msg.y = mouse_y;
                        syscall3(SYS_IPC_SEND, comp_port, (long)&msg, sizeof(msg));
                        
                        // Handle Clicks
                        // For simplicity, just sending the state in 'code' allows compositor to decide
                        // But standard way is separate events.
                        // Our compositor `evt.type == 2` check in kernel fallback was "Mouse".
                        // But IPC handler uses `msg_input_event_t`.
                        // The compositor code I read:
                        // if (evt.type == 2) { // Mouse (Legacy)
                        // But for IPC:
                        // evt.type = ievt->type;
                        // ...
                        // wait, the compositor loop for IPC doesn't have a `if (evt.type == 2)` block inside the IPC handler!
                        // It only has `if (evt.type == 1) { // Keyboard }`
                        
                        // I NEED TO FIX THE COMPOSITOR TO HANDLE MOUSE IPC EVENTS!
                    }
                    break;
            }
        }
        
        syscall3(SYS_YIELD, 0, 0, 0);
    }
}
