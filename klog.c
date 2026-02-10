#include "klog.h"
#include "spinlock.h"
#include "console.h"

#define KLOG_SIZE 16384 // 16KB log buffer

static char klog_buffer[KLOG_SIZE];
static size_t klog_head = 0; // Write index
static size_t klog_tail = 0; // Read index (if we were implementing a reader)
static int klog_full = 0;
static spinlock_t klog_lock = {0};

void klog_init(void) {
    spinlock_init(&klog_lock);
    klog_head = 0;
    klog_tail = 0;
    klog_full = 0;
    // Clear buffer
    for (int i = 0; i < KLOG_SIZE; i++) klog_buffer[i] = 0;
}

void klog_putc(char c) {
    spinlock_acquire(&klog_lock);
    
    klog_buffer[klog_head] = c;
    klog_head = (klog_head + 1) % KLOG_SIZE;
    
    if (klog_full) {
        klog_tail = (klog_tail + 1) % KLOG_SIZE; // Overwrite oldest
    }
    
    if (klog_head == klog_tail) {
        klog_full = 1;
    }
    
    spinlock_release(&klog_lock);
}

char klog_read(void) {
    spinlock_acquire(&klog_lock);
    
    if (klog_head == klog_tail && !klog_full) {
        spinlock_release(&klog_lock);
        return 0; // Empty
    }
    
    char c = klog_buffer[klog_tail];
    klog_tail = (klog_tail + 1) % KLOG_SIZE;
    klog_full = 0;
    
    spinlock_release(&klog_lock);
    return c;
}

void klog_dump(void) {
    // Dump entire buffer to console
    // Note: We don't acquire lock here to avoid deadlocks during panic
    
    size_t current = klog_tail;
    // If full, tail is head. If not full, valid data is from tail to head.
    
    int count = klog_full ? KLOG_SIZE : (klog_head >= klog_tail ? klog_head - klog_tail : KLOG_SIZE - klog_tail + klog_head);
    
    for (int i = 0; i < count; i++) {
        char c = klog_buffer[current];
        console_putc(c);
        current = (current + 1) % KLOG_SIZE;
    }
}
