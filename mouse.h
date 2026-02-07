#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

// Mouse button states
#define MOUSE_LEFT   (1 << 0)
#define MOUSE_RIGHT  (1 << 1)
#define MOUSE_MIDDLE (1 << 2)

// Initialize PS/2 mouse
void mouse_init(void);

// Get current mouse position
int mouse_get_x(void);
int mouse_get_y(void);

// Get button state
uint8_t mouse_get_buttons(void);

// Mouse handler (called from IRQ12)
void mouse_handler(void);

#endif
