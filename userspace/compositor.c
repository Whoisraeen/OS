#include <stdint.h>
#include <stddef.h>

// Syscalls
#define SYS_EXIT         0
#define SYS_WRITE        1
#define SYS_YIELD        24
#define SYS_GET_FRAMEBUFFER 15
#define SYS_GET_INPUT_EVENT 16

static inline long syscall1(long num, long arg1) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(arg1) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall3(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3) : "rcx", "r11", "memory");
    return ret;
}

// Framebuffer Info
typedef struct {
    uint64_t addr;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint32_t bpp;
} fb_info_t;

// Globals
static fb_info_t fb_info;
static uint32_t *fb_ptr;

// Helper: RGBA Color
#define RGBA(r, g, b, a) ((((uint32_t)(a)) << 24) | (((uint32_t)(r)) << 16) | (((uint32_t)(g)) << 8) | ((uint32_t)(b)))

// Helper: Blend Pixel
static uint32_t blend_pixel(uint32_t fg, uint32_t bg) {
    uint32_t alpha = (fg >> 24) & 0xFF;
    if (alpha == 0) return bg;
    if (alpha == 255) return fg;
    
    uint32_t inv_alpha = 255 - alpha;
    
    uint32_t r = (((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * inv_alpha) / 255;
    uint32_t g = (((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * inv_alpha) / 255;
    uint32_t b = ((fg & 0xFF) * alpha + (bg & 0xFF) * inv_alpha) / 255;
    
    return RGBA(r, g, b, 255);
}

// Draw Pixel
static void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || (uint64_t)x >= fb_info.width || y < 0 || (uint64_t)y >= fb_info.height) return;
    fb_ptr[y * fb_info.width + x] = color;
}

// Draw Rect
static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            put_pixel(x + dx, y + dy, color);
        }
    }
}

// Entry Point
void _start(void) {
    // 1. Get Framebuffer
    if (syscall1(SYS_GET_FRAMEBUFFER, (long)&fb_info) != 0) {
        syscall3(SYS_WRITE, 1, (long)"[COMP] Failed to get framebuffer\n", 33);
        syscall1(SYS_EXIT, 1);
    }
    
    fb_ptr = (uint32_t *)fb_info.addr;
    
    // Debug print
    // We don't have printf, just write raw bytes if needed.
    
    // 2. Clear Screen to Dark Blue
    for (uint64_t i = 0; i < fb_info.width * fb_info.height; i++) {
        fb_ptr[i] = RGBA(0, 0, 50, 255);
    }
    
    // 3. Draw a Window (Simulated)
    int win_x = 100, win_y = 100, win_w = 400, win_h = 300;
    
    // Window Body
    draw_rect(win_x, win_y, win_w, win_h, RGBA(200, 200, 200, 255));
    
    // Title Bar
    draw_rect(win_x, win_y, win_w, 30, RGBA(50, 50, 150, 255));
    
    // 4. Main Loop
    for (;;) {
        // Yield to other processes
        syscall3(SYS_YIELD, 0, 0, 0);
        
        // Simple animation: moving box
        static int box_x = 0;
        draw_rect(box_x, 500, 50, 50, RGBA(0, 255, 0, 255));
        box_x = (box_x + 1) % (fb_info.width - 50);
        
        // Delay loop (busy wait for now, better to use sleep syscall later)
        for (volatile int i = 0; i < 100000; i++);
    }
    
    syscall1(SYS_EXIT, 0);
}
