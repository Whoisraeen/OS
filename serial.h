#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

// Initialize serial port (COM1)
void serial_init(void);

// Write a character to serial
void serial_putc(char c);

// Write a string to serial
void serial_puts(const char *str);

// Formatted print to serial
void kprintf(const char *fmt, ...);

#endif
