#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <stdint.h>
#include <stddef.h>

// Color macros
#define RGBA(r, g, b, a) ((uint32_t)((a) << 24 | (r) << 16 | (g) << 8 | (b)))
#define RGB(r, g, b) RGBA(r, g, b, 255)
#define GET_A(c) (((c) >> 24) & 0xFF)
#define GET_R(c) (((c) >> 16) & 0xFF)
#define GET_G(c) (((c) >> 8) & 0xFF)
#define GET_B(c) ((c) & 0xFF)

// Alpha blend two pixels (fg over bg)
static inline uint32_t blend_pixel(uint32_t fg, uint32_t bg) {
    uint32_t a = GET_A(fg);
    if (a == 255) return fg;
    if (a == 0) return bg;
    
    uint32_t inv_a = 255 - a;
    uint32_t r = (GET_R(fg) * a + GET_R(bg) * inv_a) / 255;
    uint32_t g = (GET_G(fg) * a + GET_G(bg) * inv_a) / 255;
    uint32_t b = (GET_B(fg) * a + GET_B(bg) * inv_a) / 255;
    
    return RGB(r, g, b);
}

// Lerp for animations (0-255 range for t)
static inline int lerp(int a, int b, int t) {
    return a + ((b - a) * t) / 255;
}

// Fast 32-bit memory set
static inline void memset32(uint32_t *dest, uint32_t val, size_t count) {
    while (count--) *dest++ = val;
}

// Fast 32-bit memory copy
static inline void memcpy32(uint32_t *dest, const uint32_t *src, size_t count) {
    while (count--) *dest++ = *src++;
}

// Rectangle structure
typedef struct {
    int x, y, w, h;
} rect_t;

// Check if point is inside rect
static inline int point_in_rect(int px, int py, rect_t *r) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

// Check if two rects intersect
static inline int rects_intersect(rect_t *a, rect_t *b) {
    return !(a->x + a->w <= b->x || b->x + b->w <= a->x ||
             a->y + a->h <= b->y || b->y + b->h <= a->y);
}

// Compositor window with its own buffer
typedef struct comp_window {
    int id;
    int x, y;
    int width, height;
    uint32_t *buffer;        // Window's pixel buffer
    char title[64];
    int active;
    int focused;
    int visible;
    int z_order;             // Higher = on top
    int shadow;              // Draw shadow?
    int rounded;             // Rounded corners?
    int alpha;               // Window transparency (0-255)

    // Window states (NEW)
    int minimized;           // Window is minimized
    int maximized;           // Window is maximized
    rect_t restore_rect;     // Dimensions before maximize

    // Resize state (NEW)
    int resizing;            // Currently being resized
    int resize_edge;         // Which edge: 0=none, 1-8=edges/corners

    // Animation state (NEW)
    int animating;           // Is animation playing
    int anim_progress;       // Animation progress (0-255)
    rect_t anim_start;       // Animation start rect
    rect_t anim_target;      // Animation target rect

    struct comp_window *next;
    struct comp_window *prev;
} comp_window_t;

// Compositor state
typedef struct {
    uint32_t *back_buffer;   // Main compositing buffer
    uint32_t *front_buffer;  // Framebuffer (video memory)
    int width;
    int height;
    comp_window_t *windows;  // Linked list of windows
    comp_window_t *active;   // Currently active window
    int dirty;               // Screen needs redraw
    rect_t dirty_rect;       // Region that needs redraw
} compositor_t;

// Initialize compositor
void compositor_init(uint32_t *framebuffer, int width, int height);

// Create a window
comp_window_t *compositor_create_window(const char *title, int x, int y, int w, int h);

// Destroy a window
void compositor_destroy_window(comp_window_t *win);

// Bring window to front
void compositor_focus_window(comp_window_t *win);

// Mark region as dirty
void compositor_invalidate(rect_t *region);

// Render all windows to back buffer, then flip
void compositor_render(void);

// Draw primitives to a window's buffer
void comp_fill_rect(comp_window_t *win, int x, int y, int w, int h, uint32_t color);
void comp_draw_text(comp_window_t *win, int x, int y, const char *text, uint32_t color);

// ======= NEW: Window Management Functions =======

// Minimize/Maximize/Restore
void compositor_minimize_window(comp_window_t *win);
void compositor_maximize_window(comp_window_t *win);
void compositor_restore_window(comp_window_t *win);
void compositor_toggle_maximize(comp_window_t *win);

// Resize window (returns new edge being dragged, or 0 if none)
int compositor_check_resize_edge(comp_window_t *win, int mouse_x, int mouse_y);
void compositor_start_resize(comp_window_t *win, int edge);
void compositor_do_resize(comp_window_t *win, int mouse_x, int mouse_y);
void compositor_end_resize(comp_window_t *win);

// Window snapping
void compositor_snap_window(comp_window_t *win, int snap_type); // 0=left, 1=right, 2=top-left, 3=top-right, 4=bottom-left, 5=bottom-right

// Animations
void compositor_animate_window(comp_window_t *win, rect_t *target, int duration_ms);
void compositor_update_animations(void);  // Call per frame

#endif
