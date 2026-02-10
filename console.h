#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

// Initialize the graphical console
void console_init(void);

// Print a character
void console_putc(char c);

// Print a string
void console_puts(const char *str);

// Formatted print
void console_printf(const char *fmt, ...);

// Set text colors
void console_set_colors(uint32_t fg, uint32_t bg);

// Clear the screen
void console_clear(void);

// Enable/Disable console output (for display handover)
void console_set_enabled(int enabled);

#endif
