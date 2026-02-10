#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "../syscalls.h"

// Color Macros
#define RGBA(r, g, b, a) ((((uint32_t)(a)) << 24) | (((uint32_t)(r)) << 16) | (((uint32_t)(g)) << 8) | ((uint32_t)(b)))
#define RGB(r, g, b) RGBA(r, g, b, 255)

// Theme Colors
#define GUI_COLOR_BG       RGB(240, 240, 240)
#define GUI_COLOR_WINDOW   RGB(255, 255, 255)
#define GUI_COLOR_TEXT     RGB(0, 0, 0)
#define GUI_COLOR_BUTTON   RGB(220, 220, 220)
#define GUI_COLOR_BUTTON_HOVER RGB(200, 200, 220)
#define GUI_COLOR_BORDER   RGB(180, 180, 180)
#define GUI_COLOR_ACCENT   RGB(0, 120, 215)

// Event Types
#define GUI_EVENT_NONE        0
#define GUI_EVENT_CLICK       1
#define GUI_EVENT_MOUSE_MOVE  2
#define GUI_EVENT_REDRAW      3

// Forward Decls
typedef struct gui_widget gui_widget_t;
typedef struct gui_window gui_window_t;

// Widget Callback
typedef void (*gui_callback_t)(gui_widget_t *widget, int event, int x, int y);

// Base Widget
struct gui_widget {
    int x, y;
    int width, height;
    int type; // 0=Generic, 1=Button, 2=Label
    gui_widget_t *next;
    gui_window_t *parent;
    gui_callback_t on_event;
    
    // State
    int is_hovered;
    
    // Data
    char text[64];
};

// Window
struct gui_window {
    int id;
    int width, height;
    uint32_t *buffer;
    uint32_t shmem_id;
    uint32_t reply_port;
    gui_widget_t *widgets;
    int should_close;
};

// API
void gui_init(void);
gui_window_t *gui_create_window(const char *title, int x, int y, int w, int h);
void gui_main_loop(gui_window_t *win);

// Widgets
gui_widget_t *gui_create_button(gui_window_t *win, int x, int y, int w, int h, const char *text, gui_callback_t cb);
gui_widget_t *gui_create_label(gui_window_t *win, int x, int y, const char *text);

// Drawing Primitives (Internal but exposed)
void gui_draw_rect(gui_window_t *win, int x, int y, int w, int h, uint32_t color);
void gui_draw_text(gui_window_t *win, int x, int y, const char *text, uint32_t color);

#endif
