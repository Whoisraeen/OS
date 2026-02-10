#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>
#include <stddef.h>

// Initialize the kernel log buffer
void klog_init(void);

// Write a character to the kernel log
void klog_putc(char c);

// Read a character from the kernel log (for debugging/dmesg)
// Returns 0 if empty
char klog_read(void);

// Dump the entire log to the console (for panic)
void klog_dump(void);

#endif
