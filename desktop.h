#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>
#include <stddef.h>

// Window structure
typedef struct {
    int x, y;           // Position
    int width, height;  // Size
    char title[64];     // Window title
    uint32_t *buffer;   // Window content buffer (optional)
    int active;         // Is this window slot in use?
    int focused;        // Is this window focused?
} window_t;

// Maximum windows
#define MAX_WINDOWS 8

// Initialize desktop
void desktop_init(void);

// Main desktop render loop (call periodically)
void desktop_render(void);

// Create a new window
int window_create(const char *title, int x, int y, int width, int height);

// Close a window
void window_close(int id);

// Draw text in a window
void window_draw_text(int id, int x, int y, const char *text, uint32_t color);

// Fill window background
void window_fill(int id, uint32_t color);

#endif
