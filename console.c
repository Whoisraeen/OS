#include "console.h"
#include "font.h"
#include "serial.h"
#include "spinlock.h"
#include <stddef.h>
#include <stdarg.h>

// External framebuffer globals from kernel.c
extern uint32_t *fb_ptr;
extern uint64_t fb_width;
extern uint64_t fb_height;

// Console state
static size_t cursor_x = 0;
static size_t cursor_y = 0;
static size_t console_cols = 0;
static size_t console_rows = 0;
static uint32_t fg_color = 0xFFFFFFFF; // White
static uint32_t bg_color = 0xFF003366; // Sony Blue
static int console_enabled = 1; // Default enabled
static spinlock_t console_lock = {0};

void console_init(void) {
    spinlock_init(&console_lock);
    if (fb_ptr == NULL || fb_width == 0 || fb_height == 0) {
        return;
    }
    
    console_cols = fb_width / FONT_WIDTH;
    console_rows = fb_height / FONT_HEIGHT;
    cursor_x = 0;
    cursor_y = 0;
    console_enabled = 1;
}

void console_set_enabled(int enabled) {
    console_enabled = enabled;
}

void console_set_colors(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

void console_clear(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    spinlock_acquire(&console_lock);
    if (fb_ptr == NULL) {
        spinlock_release(&console_lock);
        __asm__ volatile("push %0; popfq" : : "r"(rflags));
        return;
    }
    
    for (size_t i = 0; i < fb_width * fb_height; i++) {
        fb_ptr[i] = bg_color;
    }
    cursor_x = 0;
    cursor_y = 0;
    spinlock_release(&console_lock);
    __asm__ volatile("push %0; popfq" : : "r"(rflags));
}

static void console_scroll(void) {
    if (fb_ptr == NULL) return;
    
    // Move all lines up by one
    size_t line_size = fb_width * FONT_HEIGHT;
    
    // Copy lines 1..n to 0..n-1
    for (size_t y = 0; y < fb_height - FONT_HEIGHT; y++) {
        for (size_t x = 0; x < fb_width; x++) {
            fb_ptr[y * fb_width + x] = fb_ptr[(y + FONT_HEIGHT) * fb_width + x];
        }
    }
    
    // Clear the last line
    for (size_t y = fb_height - FONT_HEIGHT; y < fb_height; y++) {
        for (size_t x = 0; x < fb_width; x++) {
            fb_ptr[y * fb_width + x] = bg_color;
        }
    }
}

static void draw_char(char c, size_t x, size_t y) {
    if (fb_ptr == NULL) return;
    
    // Get glyph index
    int index = (int)c - FONT_FIRST_CHAR;
    if (index < 0 || index > (FONT_LAST_CHAR - FONT_FIRST_CHAR)) {
        index = 0; // Use space for invalid characters
    }
    
    const uint8_t *glyph = font_data[index];
    
    // Draw the glyph
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            size_t px = x + col;
            size_t py = y + row;
            
            if (px < fb_width && py < fb_height) {
                uint32_t color = (bits & (0x80 >> col)) ? fg_color : bg_color;
                fb_ptr[py * fb_width + px] = color;
            }
        }
    }
}

void console_putc(char c) {
    // NOTE: serial_putc is NOT called here. Callers that need serial output
    // (kprintf, console_dev_write) call serial_putc explicitly.
    // This prevents double serial output.
    
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    spinlock_acquire(&console_lock);

    // If console is disabled (GUI owns screen) or not initialized, stop here.
    if (fb_ptr == NULL || !console_enabled) {
        spinlock_release(&console_lock);
        __asm__ volatile("push %0; popfq" : : "r"(rflags));
        return;
    }
    
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 4) & ~3;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            draw_char(' ', cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT);
        }
    } else if (c >= FONT_FIRST_CHAR && c <= FONT_LAST_CHAR) {
        draw_char(c, cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT);
        cursor_x++;
    }
    
    // Handle wrap
    if (cursor_x >= console_cols) {
        cursor_x = 0;
        cursor_y++;
    }
    
    // Handle scroll
    if (cursor_y >= console_rows) {
        console_scroll();
        cursor_y = console_rows - 1;
    }
    spinlock_release(&console_lock);
    __asm__ volatile("push %0; popfq" : : "r"(rflags));
}

void console_puts(const char *str) {
    while (*str) {
        console_putc(*str++);
    }
}

// Number to string
static void print_num(uint64_t num, int base, int width, char pad) {
    char buf[32];
    int i = 0;
    
    if (num == 0) {
        buf[i++] = '0';
    } else {
        while (num > 0) {
            int digit = num % base;
            buf[i++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
            num /= base;
        }
    }
    
    while (i < width) {
        console_putc(pad);
        width--;
    }
    
    while (i > 0) {
        console_putc(buf[--i]);
    }
}

void console_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            
            char pad = ' ';
            int width = 0;
            
            if (*fmt == '0') {
                pad = '0';
                fmt++;
            }
            
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
            
            switch (*fmt) {
                case 'd':
                case 'i': {
                    int val = va_arg(args, int);
                    if (val < 0) {
                        console_putc('-');
                        val = -val;
                    }
                    print_num((uint64_t)val, 10, width, pad);
                    break;
                }
                case 'u': {
                    unsigned int val = va_arg(args, unsigned int);
                    print_num((uint64_t)val, 10, width, pad);
                    break;
                }
                case 'x': {
                    unsigned int val = va_arg(args, unsigned int);
                    print_num((uint64_t)val, 16, width, pad);
                    break;
                }
                case 'l': {
                    fmt++;
                    if (*fmt == 'x') {
                        uint64_t val = va_arg(args, uint64_t);
                        print_num(val, 16, width, pad);
                    } else if (*fmt == 'u') {
                        uint64_t val = va_arg(args, uint64_t);
                        print_num(val, 10, width, pad);
                    }
                    break;
                }
                case 'p': {
                    uint64_t val = (uint64_t)va_arg(args, void *);
                    console_puts("0x");
                    print_num(val, 16, 16, '0');
                    break;
                }
                case 's': {
                    const char *str = va_arg(args, const char *);
                    if (str) {
                        console_puts(str);
                    } else {
                        console_puts("(null)");
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    console_putc(c);
                    break;
                }
                case '%':
                    console_putc('%');
                    break;
                default:
                    console_putc('%');
                    console_putc(*fmt);
                    break;
            }
        } else {
            console_putc(*fmt);
        }
        fmt++;
    }
    
    va_end(args);
}
