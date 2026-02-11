#include <stdint.h>
#include "font.h"
#include "gui.h"
#include "keymap.h"
#include "syscalls.h"

// Freestanding definitions
typedef unsigned long size_t;

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for ( ; i < n; i++)
        dest[i] = '\0';
    return dest;
}

void printf(const char *fmt, ...) {
    // Determine length
    int len = 0;
    while (fmt[len]) len++;
    syscall3(SYS_WRITE, 1, (uint64_t)fmt, len);
}

void exit(int code) {
    syscall1(SYS_EXIT, code);
    while(1);
}
// IPC Constants
#define IPC_PORT_FLAG_RECEIVE (1 << 1)
#define IPC_PORT_FLAG_SEND    (1 << 2)
#define IPC_RECV_NONBLOCK     (1 << 0)

// IPC Message Structure
typedef struct {
    uint32_t msg_id;
    uint32_t sender_pid;
    uint32_t reply_port;
    uint32_t size;
    uint64_t timestamp;
    uint8_t data[128];
} ipc_message_t;


// Utils moved to u_stdlib.h

// Helper to draw text to buffer
static void draw_char(uint32_t *buffer, int width, int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 126) return;
    int idx = c - 32;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font_data[idx][row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                buffer[(y + row) * width + (x + col)] = color;
            }
        }
    }
}

static void draw_string(uint32_t *buffer, int width, int x, int y, const char *str, uint32_t color) {
    while (*str) {
        draw_char(buffer, width, x, y, *str, color);
        x += FONT_WIDTH;
        str++;
    }
}

int main(void) {
    printf("[INIT] Hello from Userspace! Spawning Compositor...\n");
    
    // 1. Spawn Compositor
    long pid = syscall1(SYS_PROC_EXEC, (long)"compositor.elf");
    if (pid < 0) {
        printf("[INIT] Failed to spawn compositor!\n");
        exit(1);
    }
    
    // 2. Wait for Compositor Service
    long comp_port = 0;
    for (int i = 0; i < 100; i++) { // Try for a while
        comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
        if (comp_port > 0) break;
        
        // Busy wait / yield
        for (volatile int j = 0; j < 100000; j++); 
        syscall3(SYS_YIELD, 0, 0, 0);
    }
    
    // Spawn Drivers & Panel
    syscall1(SYS_PROC_EXEC, (long)"keyboard_driver.elf");
    syscall1(SYS_PROC_EXEC, (long)"mouse_driver.elf");
    syscall1(SYS_PROC_EXEC, (long)"panel.elf");

    if (comp_port > 0) {
        syscall3(SYS_WRITE, 1, (long)"[INIT] Compositor found, requesting window...\n", 46);
        
        // 3. Create Reply Port for Events
        long my_port = syscall1(SYS_IPC_CREATE, IPC_PORT_FLAG_RECEIVE);
        
        int win_w = 300;
        int win_h = 200;
        
        // 4. Create Shared Memory for Window Content
        // Size = w * h * 4 bytes (32bpp)
        size_t size = win_w * win_h * 4;
        long shmem_id = syscall2(SYS_IPC_SHMEM_CREATE, size, 0); // 0 = default flags
        
        if (shmem_id > 0) {
            uint32_t *buffer = (uint32_t *)syscall1(SYS_IPC_SHMEM_MAP, shmem_id);
            
            if (buffer) {
                // Draw something to the buffer
                for (int i = 0; i < win_w * win_h; i++) buffer[i] = 0xFFCCCCCC; // Light gray
                
                draw_string(buffer, win_w, 10, 10, "Init Process", 0xFF000000);
                draw_string(buffer, win_w, 10, 30, "Status: Running", 0xFF008800);
                draw_string(buffer, win_w, 10, 180, "Click me!", 0xFF555555);
                
                // 5. Create Window via IPC
                ipc_message_t req;
                req.msg_id = MSG_CREATE_WINDOW;
                req.size = sizeof(msg_create_window_t);
                req.reply_port = my_port; // Tell compositor where to send events
                
                msg_create_window_t *win_req = (msg_create_window_t *)req.data;
                win_req->x = 200;
                win_req->y = 200;
                win_req->w = win_w;
                win_req->h = win_h;
                win_req->shmem_id = (uint32_t)shmem_id;
                win_req->reply_port = (uint32_t)my_port;
                strncpy(win_req->title, "Init Status Window", 32);
                
                syscall3(SYS_IPC_SEND, comp_port, (long)&req, 0);
                
                syscall3(SYS_WRITE, 1, (long)"[INIT] Window request sent with shmem.\n", 40);
                
                // 6. Event Loop
                ipc_message_t msg;
                int running = 1;
                uint32_t text_color = 0xFF008800;
                
                // Keyboard buffer
                char kbd_buffer[64];
                int kbd_len = 0;
                memset(kbd_buffer, 0, 64);
                
                // Keyboard State
                int shift_pressed = 0;
                
                while (running) {
                    long res = syscall3(SYS_IPC_RECV, my_port, (long)&msg, IPC_RECV_NONBLOCK);
                    if (res == 0) {
                        if (msg.msg_id == MSG_INPUT_EVENT) {
                            msg_input_event_t *evt = (msg_input_event_t *)msg.data;
                            
                            if (evt->type == EVENT_MOUSE_DOWN) {
                                // Toggle color
                                if (text_color == 0xFF008800) text_color = 0xFF000088;
                                else text_color = 0xFF008800;
                                
                                // Redraw
                                for (int i = 0; i < win_w * win_h; i++) buffer[i] = 0xFFCCCCCC;
                                draw_string(buffer, win_w, 10, 10, "Init Process", 0xFF000000);
                                draw_string(buffer, win_w, 10, 30, "Status: Clicked!", text_color);
                                
                                char coords[32] = "Pos: ";
                                // Simple integer to string (very basic)
                                int x = evt->x;
                                int idx = 5;
                                if (x >= 100) { coords[idx++] = '0' + (x/100); x %= 100; }
                                if (x >= 10)  { coords[idx++] = '0' + (x/10); x %= 10; }
                                coords[idx++] = '0' + x;
                                coords[idx] = 0;
                                draw_string(buffer, win_w, 10, 50, coords, 0xFF000000);
                                
                                draw_string(buffer, win_w, 10, 70, "Type: ", 0xFF000000);
                                draw_string(buffer, win_w, 58, 70, kbd_buffer, 0xFF000000);
                            } else if (evt->type == EVENT_KEY_DOWN) {
                                uint32_t scancode = evt->code;
                                
                                // Handle Shift
                                if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; continue; }
                                if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; continue; }
                                
                                if (scancode & 0x80) {
                                    // Release
                                } else {
                                    // Press
                                    char c = shift_pressed ? kbdus_upper[scancode] : kbdus_lower[scancode];
                                    
                                    if (c >= 32 && c <= 126 && kbd_len < 63) {
                                        kbd_buffer[kbd_len++] = c;
                                        kbd_buffer[kbd_len] = 0;
                                    } else if (c == '\b' && kbd_len > 0) { // Backspace
                                        kbd_len--;
                                        kbd_buffer[kbd_len] = 0;
                                    }
                                    
                                    // Redraw
                                    for (int i = 0; i < win_w * win_h; i++) buffer[i] = 0xFFCCCCCC;
                                    draw_string(buffer, win_w, 10, 10, "Init Process", 0xFF000000);
                                    draw_string(buffer, win_w, 10, 30, "Status: Typing...", text_color);
                                    draw_string(buffer, win_w, 10, 70, "Type: ", 0xFF000000);
                                    draw_string(buffer, win_w, 58, 70, kbd_buffer, 0xFF000000);
                                }
                            }
                        }
                    }
                    
                    syscall3(SYS_YIELD, 0, 0, 0);
                }
                
            } else {
                 printf("[INIT] Failed to map shared memory.\n");
            }
        } else {
            printf("[INIT] Failed to create shared memory.\n");
        }

    } else {
        printf("[INIT] Compositor service timeout.\n");
    }
    
    // Fallback loop if something failed
    for (;;) {
        syscall3(SYS_YIELD, 0, 0, 0);
    }
    
    return 0;
}
