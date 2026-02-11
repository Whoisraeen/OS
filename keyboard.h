#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
int get_keyboard_event(uint32_t *type, uint32_t *code);

#endif
