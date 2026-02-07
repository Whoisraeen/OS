#include "mouse.h"
#include "io.h"
#include "serial.h"

// Mouse state
static int mouse_x = 512;  // Start at center
static int mouse_y = 384;
static uint8_t mouse_buttons = 0;

// Mouse packet state
static uint8_t mouse_cycle = 0;
static int8_t mouse_bytes[3];

// Screen bounds
extern uint64_t fb_width;
extern uint64_t fb_height;

// Wait for mouse controller
static void mouse_wait(int type) {
    int timeout = 100000;
    if (type == 0) {
        // Wait for input buffer to be clear
        while (timeout-- && (inb(0x64) & 2));
    } else {
        // Wait for output buffer to be full
        while (timeout-- && !(inb(0x64) & 1));
    }
}

// Write to mouse
static void mouse_write(uint8_t data) {
    mouse_wait(0);
    outb(0x64, 0xD4);  // Tell controller to send to mouse
    mouse_wait(0);
    outb(0x60, data);
}

// Read from mouse
static uint8_t mouse_read(void) {
    mouse_wait(1);
    return inb(0x60);
}

void mouse_init(void) {
    uint8_t status;
    
    // Enable auxiliary device (mouse)
    mouse_wait(0);
    outb(0x64, 0xA8);
    
    // Enable interrupts
    mouse_wait(0);
    outb(0x64, 0x20);  // Get compaq status byte
    mouse_wait(1);
    status = inb(0x60);
    status |= 2;       // Enable IRQ12
    status &= ~0x20;   // Enable mouse clock
    mouse_wait(0);
    outb(0x64, 0x60);  // Set compaq status byte
    mouse_wait(0);
    outb(0x60, status);
    
    // Reset mouse
    mouse_write(0xFF);
    mouse_read();  // ACK
    mouse_read();  // Self-test result (0xAA)
    mouse_read();  // Mouse ID (0x00)
    
    // Use default settings
    mouse_write(0xF6);
    mouse_read();  // Acknowledge
    
    // Set sample rate to 100
    mouse_write(0xF3);
    mouse_read();
    mouse_write(100);
    mouse_read();
    
    // Enable data reporting
    mouse_write(0xF4);
    mouse_read();  // Acknowledge
    
    kprintf("[MOUSE] PS/2 mouse initialized\n");
}

// Mouse event buffer
#define MOUSE_BUF_SIZE 32
typedef struct {
    uint8_t buttons;
    int32_t x;
    int32_t y;
} mouse_event_t;

static mouse_event_t mouse_buf[MOUSE_BUF_SIZE];
static int mouse_head = 0;
static int mouse_tail = 0;

int get_mouse_event(uint32_t *type, uint32_t *buttons, int32_t *x, int32_t *y) {
    if (mouse_head == mouse_tail) return 0;
    
    *type = 2; // Mouse
    *buttons = mouse_buf[mouse_tail].buttons;
    *x = mouse_buf[mouse_tail].x;
    *y = mouse_buf[mouse_tail].y;
    mouse_tail = (mouse_tail + 1) % MOUSE_BUF_SIZE;
    return 1;
}

void mouse_handler(void) {
    static int packet_count = 0;
    static int raw_count = 0;
    uint8_t data = inb(0x60);
    
    // Debug: Log first 20 bytes received
    raw_count++;
    if (raw_count <= 20) {
        kprintf("[MOUSE] byte %d: 0x%02x cycle=%d\n", raw_count, data, mouse_cycle);
    }
    
    switch (mouse_cycle) {
        case 0:
            mouse_bytes[0] = data;
            // Bit 3 should always be set in first byte of a valid packet
            if ((data & 0x08) == 0) {
                // If not, we are likely out of sync. Reset.
                mouse_cycle = 0;
                return;
            }
            mouse_cycle++;
            break;
        case 1:
            mouse_bytes[1] = data;
            mouse_cycle++;
            break;
        case 2:
            mouse_bytes[2] = data;
            mouse_cycle = 0;
            
            // Decode packet
            uint8_t state = mouse_bytes[0];
            int dx = (int)mouse_bytes[1];
            int dy = (int)mouse_bytes[2];
            
            // Handle sign extension if needed
            if (state & 0x10) dx |= 0xFFFFFF00;
            if (state & 0x20) dy |= 0xFFFFFF00;
            
            // Update position
            mouse_x += dx;
            mouse_y -= dy; // Y is inverted on PS/2
            
            // Clamp to screen
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= (int)fb_width) mouse_x = fb_width - 1;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= (int)fb_height) mouse_y = fb_height - 1;
            
            mouse_buttons = state & 0x07;
            
            packet_count++;
            
            // Push to buffer
            int next = (mouse_head + 1) % MOUSE_BUF_SIZE;
            if (next != mouse_tail) {
                mouse_buf[mouse_head].buttons = mouse_buttons;
                mouse_buf[mouse_head].x = mouse_x;
                mouse_buf[mouse_head].y = mouse_y;
                mouse_head = next;
            }
            
            break;
    }
}

int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
uint8_t mouse_get_buttons(void) { return mouse_buttons; }
