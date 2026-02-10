#ifndef KLOG_H
#define KLOG_H

#include <stddef.h>
#include <stdint.h>

// Initialize the kernel log buffer
void klog_init(void);

// Write a character to the kernel log
void klog_putc(char c);

// Write a string to the kernel log
void klog_puts(const char *str);

// Read from the kernel log (for userspace/dmesg)
// Returns number of bytes read
size_t klog_read(char *buf, size_t len, size_t offset);

// Dump kernel log to console (used in panic)
void klog_dump(void);

#endif
