#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>

void keyboard_init(void);
int  get_keyboard_event(uint32_t *type, uint32_t *code);

// Blocking line-buffered ASCII read (for stdin)
size_t keyboard_read_ascii(uint8_t *buf, size_t count);

// Non-blocking: returns 1 if character(s) available
int keyboard_ascii_available(void);

#endif
