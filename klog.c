#include "klog.h"
#include "spinlock.h"
#include "string.h"
#include <stdbool.h>

#define KLOG_SIZE 65536 // 64KB Buffer

static char klog_buffer[KLOG_SIZE];
static size_t klog_head = 0; // Write pointer
static size_t klog_tail = 0; // Read pointer (if we were consuming, but we act as a ring)
static bool klog_full = false;
static spinlock_t klog_lock;

void klog_init(void) {
    spinlock_init(&klog_lock);
    klog_head = 0;
    klog_tail = 0;
    klog_full = false;
    memset(klog_buffer, 0, KLOG_SIZE);
}

static bool klog_active = true;

// Define external console_putc to avoid header loop if needed, 
// or just include console.h if safe. console.h depends on nothing circular?
// console.h -> stdint, stddef. Safe.
#include "console.h" 

void klog_putc(char c) {
    if (!klog_active) return; // Prevent recursion during dump

    spinlock_acquire(&klog_lock);
    
    klog_buffer[klog_head] = c;
    klog_head = (klog_head + 1) % KLOG_SIZE;
    
    if (klog_full) {
        klog_tail = (klog_tail + 1) % KLOG_SIZE;
    } else if (klog_head == klog_tail) {
        klog_full = true;
    }
    
    spinlock_release(&klog_lock);
}

void klog_puts(const char *str) {
    while (*str) {
        klog_putc(*str++);
    }
}

size_t klog_read(char *buf, size_t len, size_t offset) {
    spinlock_acquire(&klog_lock);
    
    // Calculate actual number of bytes available
    size_t available = 0;
    if (klog_full) {
        available = KLOG_SIZE;
    } else {
        if (klog_head >= klog_tail) {
            available = klog_head - klog_tail;
        } else {
            available = KLOG_SIZE - (klog_tail - klog_head);
        }
    }
    
    if (offset >= available) {
        spinlock_release(&klog_lock);
        return 0;
    }
    
    size_t to_read = len;
    if (offset + to_read > available) {
        to_read = available - offset;
    }
    
    // Read from (tail + offset) % SIZE
    size_t start_idx = (klog_tail + offset) % KLOG_SIZE;
    
    for (size_t i = 0; i < to_read; i++) {
        buf[i] = klog_buffer[(start_idx + i) % KLOG_SIZE];
    }
    
    spinlock_release(&klog_lock);
    return to_read;
}

void klog_dump(void) {
    klog_active = false;
    
    spinlock_acquire(&klog_lock);
    
    size_t count = 0;
    if (klog_full) count = KLOG_SIZE;
    else if (klog_head >= klog_tail) count = klog_head - klog_tail;
    else count = KLOG_SIZE - (klog_tail - klog_head);
    
    size_t idx = klog_tail;
    for (size_t i = 0; i < count; i++) {
        char c = klog_buffer[idx];
        // Print directly to console/serial via console_putc
        // Since klog_active is false, it won't re-log
        console_putc(c);
        
        idx = (idx + 1) % KLOG_SIZE;
    }
    
    spinlock_release(&klog_lock);
    klog_active = true;
}
