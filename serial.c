#include "serial.h"
#include "io.h"
#include "spinlock.h"
#include "klog.h"
#include "console.h"

#define COM1_PORT 0x3F8

static spinlock_t serial_lock;

// Check if transmit buffer is empty
static int serial_is_transmit_empty(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

void serial_init(void) {
    spinlock_init(&serial_lock);

    // Disable interrupts
    outb(COM1_PORT + 1, 0x00);
    
    // Enable DLAB (set baud rate divisor)
    outb(COM1_PORT + 3, 0x80);
    
    // Set divisor to 3 (lo byte) 38400 baud
    outb(COM1_PORT + 0, 0x03);
    // (hi byte)
    outb(COM1_PORT + 1, 0x00);
    
    // 8 bits, no parity, one stop bit
    outb(COM1_PORT + 3, 0x03);
    
    // Enable FIFO, clear them, with 14-byte threshold
    outb(COM1_PORT + 2, 0xC7);
    
    // IRQs enabled, RTS/DSR set
    outb(COM1_PORT + 4, 0x0B);
    
    // Set in loopback mode to test the serial chip
    outb(COM1_PORT + 4, 0x1E);
    
    // Test by sending byte 0xAE
    outb(COM1_PORT + 0, 0xAE);
    
    // Check if we received the same byte
    if (inb(COM1_PORT + 0) != 0xAE) {
        // Serial port is faulty (or doesn't exist in emulator)
        return;
    }
    
    // Set normal operation mode (not loopback, IRQs enabled, OUT#1 and OUT#2 bits enabled)
    outb(COM1_PORT + 4, 0x0F);
}

void serial_putc(char c) {
    while (!serial_is_transmit_empty());
    outb(COM1_PORT, c);
}

void serial_puts(const char *str) {
    while (*str) {
        if (*str == '\n') {
            serial_putc('\r');
        }
        serial_putc(*str++);
    }
}

// Simple number to string conversion
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
    
    // Pad
    while (i < width) {
        char c = pad;
        klog_putc(c);
        serial_putc(c);
        console_putc(c);
        width--;
    }
    
    // Print in reverse
    while (i > 0) {
        char c = buf[--i];
        klog_putc(c);
        serial_putc(c);
        console_putc(c);
    }
}

void kprintf(const char *fmt, ...) {
    spinlock_acquire(&serial_lock);
    
    va_list args;
    va_start(args, fmt);
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            
            // Parse width and padding
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
                        klog_putc('-');
                        serial_putc('-');
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
                        unsigned long val = va_arg(args, unsigned long);
                        print_num((uint64_t)val, 16, width, pad);
                    } else if (*fmt == 'u') {
                        unsigned long val = va_arg(args, unsigned long);
                        print_num((uint64_t)val, 10, width, pad);
                    } else if (*fmt == 'd') {
                         long val = va_arg(args, long);
                         if (val < 0) {
                            klog_putc('-');
                            serial_putc('-');
                            console_putc('-');
                            val = -val;
                        }
                        print_num((uint64_t)val, 10, width, pad);
                    }
                    break;
                }
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (!s) s = "(null)";
                    while (*s) {
                        klog_putc(*s);
                        serial_putc(*s);
                        console_putc(*s);
                        s++;
                    }
                    break;
                }
                case 'c': {
                    int c = va_arg(args, int);
                    klog_putc((char)c);
                    serial_putc((char)c);
                    console_putc((char)c);
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void *);
                    klog_putc('0'); klog_putc('x');
                    serial_putc('0'); serial_putc('x');
                    console_putc('0'); console_putc('x');
                    print_num((uint64_t)ptr, 16, 0, ' ');
                    break;
                }
                case '%': {
                    klog_putc('%');
                    serial_putc('%');
                    console_putc('%');
                    break;
                }
            }
        } else {
            klog_putc(*fmt);
            serial_putc(*fmt);
            console_putc(*fmt);
        }
        fmt++;
    }
    
    va_end(args);
    spinlock_release(&serial_lock);
}
