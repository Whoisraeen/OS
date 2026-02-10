#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "../syscalls.h"

#define RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define COLOR_WHITE RGBA(255, 255, 255, 255)
#define COLOR_BLACK RGBA(0, 0, 0, 255)
#define COLOR_RED   RGBA(255, 0, 0, 255)
#define COLOR_GREEN RGBA(0, 255, 0, 255)
#define COLOR_BLUE  RGBA(0, 0, 255, 255)
#define COLOR_GRAY  RGBA(128, 128, 128, 255)

// Widget Types
typedef enum {
    WIDGET_WINDOW,
    WIDGET_BUTTON,
    WIDGET_LABEL,
    WIDGET_PANEL
} widget_type_t;

// Forward declaration
struct gui_widget;

// Event Handler Typedef
typedef void (*event_handler_t)(struct gui_widget *widget, void *event_data);

// Base Widget Structure
typedef struct gui_widget {
    widget_type_t type;
    int x, y, w, h;
    struct gui_widget *parent;
    struct gui_widget *first_child;
    struct gui_widget *next_sibling;
    
    // Event Handlers
    event_handler_t on_click;
    
    // Internal
    void *priv_data; // For subclass data
} gui_widget_t;

// Window Structure (Inherits Widget)
typedef struct {
    gui_widget_t base;
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t *buffer;
    uint32_t shmem_id;
    int event_port;
    char title[64];
    uint32_t bg_color;
    int should_close;
} gui_window_t;

// Button Structure
typedef struct {
    gui_widget_t base;
    char text[32];
    uint32_t color;
    uint32_t hover_color;
    uint32_t text_color;
    int is_hovered;
    int is_pressed;
} gui_button_t;

// Label Structure
typedef struct {
    gui_widget_t base;
    char text[64];
    uint32_t text_color;
} gui_label_t;

// API
void gui_init(void);

// Window Management
gui_window_t *gui_create_window(const char *title, int width, int height);
void gui_window_add_child(gui_window_t *win, gui_widget_t *child);
void gui_window_update(gui_window_t *win);
int gui_window_process_events(gui_window_t *win); // Returns 0 if closed

// Widget Creation
gui_button_t *gui_create_button(int x, int y, int w, int h, const char *text, event_handler_t on_click);
gui_label_t *gui_create_label(int x, int y, const char *text);

// Drawing Primitives (Low Level)
void gui_draw_rect(gui_window_t *win, int x, int y, int w, int h, uint32_t color);
void gui_draw_text(gui_window_t *win, int x, int y, const char *text, uint32_t color);

#endif
