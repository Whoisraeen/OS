#include "compositor.h"
#include "font.h"
#include "heap.h"
#include "serial.h"
#include "mouse.h"

// Framebuffer access
extern uint32_t *fb_ptr;
extern uint64_t fb_width;
extern uint64_t fb_height;

// Global compositor
static compositor_t comp;
static comp_window_t window_pool[16];
static int next_window_id = 0;

// Theme colors
#define THEME_BG        RGBA(30, 58, 95, 255)      // Desktop blue
#define THEME_WINDOW    RGBA(32, 32, 32, 230)      // Semi-transparent dark
#define THEME_TITLEBAR  RGBA(45, 45, 48, 255)      // Dark gray
#define THEME_TITLE_FOC RGBA(0, 122, 204, 255)     // Focused blue
#define THEME_SHADOW    RGBA(0, 0, 0, 80)          // Soft shadow
#define THEME_ACCENT    RGBA(0, 150, 255, 255)     // Accent color

// Rounded corner radius
#define CORNER_RADIUS 8
#define SHADOW_OFFSET 6
#define SHADOW_SIZE   8

// String copy
static void strcopy(char *dest, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

// Check if pixel is inside rounded rect
static int inside_rounded_rect(int px, int py, int w, int h, int r) {
    // Inside main rect (excluding corners)
    if (px >= r && px < w - r) return 1;
    if (py >= r && py < h - r) return 1;
    
    // Check corners
    int cx, cy;
    if (px < r && py < r) {
        cx = r; cy = r;
    } else if (px >= w - r && py < r) {
        cx = w - r - 1; cy = r;
    } else if (px < r && py >= h - r) {
        cx = r; cy = h - r - 1;
    } else if (px >= w - r && py >= h - r) {
        cx = w - r - 1; cy = h - r - 1;
    } else {
        return 1;
    }
    
    int dx = px - cx;
    int dy = py - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

// Draw pixel with bounds check and alpha
static void put_pixel_alpha(int x, int y, uint32_t color) {
    if (x < 0 || x >= comp.width || y < 0 || y >= comp.height) return;
    uint32_t bg = comp.back_buffer[y * comp.width + x];
    comp.back_buffer[y * comp.width + x] = blend_pixel(color, bg);
}

// Draw filled rect with alpha
static void draw_rect_alpha(int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            put_pixel_alpha(x + dx, y + dy, color);
        }
    }
}

// Draw rounded rect with alpha
static void draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            if (inside_rounded_rect(dx, dy, w, h, r)) {
                put_pixel_alpha(x + dx, y + dy, color);
            }
        }
    }
}

// Draw shadow behind a rect
static void draw_shadow(int x, int y, int w, int h) {
    // Simple soft shadow - offset and multiple layers
    for (int layer = SHADOW_SIZE; layer > 0; layer--) {
        int alpha = (80 * layer) / SHADOW_SIZE;
        uint32_t shadow = RGBA(0, 0, 0, alpha);
        int off = SHADOW_OFFSET + (SHADOW_SIZE - layer);
        
        // Draw just the edges for the offset shadow
        for (int dx = 0; dx < w; dx++) {
            put_pixel_alpha(x + dx + off, y + h + off - layer, shadow);
        }
        for (int dy = 0; dy < h; dy++) {
            put_pixel_alpha(x + w + off - layer, y + dy + off, shadow);
        }
    }
    
    // Main shadow body
    draw_rect_alpha(x + SHADOW_OFFSET, y + SHADOW_OFFSET, w, h, RGBA(0, 0, 0, 40));
}

// Draw character with alpha
static void draw_char_alpha(int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 126) return;
    int idx = c - 32;
    
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font_data[idx][row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                put_pixel_alpha(x + col, y + row, color);
            }
        }
    }
}

// Draw string with alpha
static void draw_string_alpha(int x, int y, const char *str, uint32_t color) {
    while (*str) {
        draw_char_alpha(x, y, *str, color);
        x += FONT_WIDTH;
        str++;
    }
}

// Draw cursor (arrow shape)
static void draw_cursor(int mx, int my) {
    static const uint8_t cursor[12] = {
        0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF, 0xFC, 0xFC, 0xCC, 0x06, 0x06
    };
    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 8; col++) {
            if (cursor[row] & (0x80 >> col)) {
                put_pixel_alpha(mx + col, my + row, 0xFFFFFFFF);
            }
        }
    }
}

// Draw window with all effects
static void draw_window(comp_window_t *win) {
    if (!win->active || !win->visible) return;
    
    int x = win->x;
    int y = win->y;
    int w = win->width;
    int h = win->height;
    int r = win->rounded ? CORNER_RADIUS : 0;
    
    // Draw shadow first (behind window)
    if (win->shadow) {
        draw_shadow(x, y, w, h);
    }
    
    // Title bar
    uint32_t title_color = win->focused ? THEME_TITLE_FOC : THEME_TITLEBAR;
    
    // Draw window background (semi-transparent)
    uint32_t win_color = RGBA(40, 40, 40, win->alpha);
    
    // Draw rounded titlebar
    if (r > 0) {
        // Top rounded part
        for (int dy = 0; dy < 28; dy++) {
            for (int dx = 0; dx < w; dx++) {
                if (inside_rounded_rect(dx, dy, w, 28 + r, r)) {
                    put_pixel_alpha(x + dx, y + dy, title_color);
                }
            }
        }
        // Body with rounded bottom
        for (int dy = 28; dy < h; dy++) {
            for (int dx = 0; dx < w; dx++) {
                if (dy < h - r || inside_rounded_rect(dx, dy - (h - r - r), w, r * 2, r)) {
                    put_pixel_alpha(x + dx, y + dy, win_color);
                }
            }
        }
    } else {
        draw_rect_alpha(x, y, w, 28, title_color);
        draw_rect_alpha(x, y + 28, w, h - 28, win_color);
    }
    
    // Title text
    draw_string_alpha(x + 10, y + 8, win->title, 0xFFFFFFFF);
    
    // Close button (X)
    int bx = x + w - 28;
    int by = y + 4;
    draw_rect_alpha(bx, by, 20, 20, RGBA(200, 50, 50, 200));
    draw_string_alpha(bx + 6, by + 4, "X", 0xFFFFFFFF);
    
    // Draw window content buffer if exists
    if (win->buffer) {
        for (int dy = 0; dy < h - 28; dy++) {
            for (int dx = 0; dx < w; dx++) {
                uint32_t pixel = win->buffer[dy * w + dx];
                if (GET_A(pixel) > 0) {
                    put_pixel_alpha(x + dx, y + 28 + dy, pixel);
                }
            }
        }
    }
}

void compositor_init(uint32_t *framebuffer, int width, int height) {
    comp.front_buffer = framebuffer;
    comp.width = width;
    comp.height = height;
    comp.windows = NULL;
    comp.active = NULL;
    comp.dirty = 1;
    
    // Allocate back buffer
    size_t buffer_size = width * height * sizeof(uint32_t);
    comp.back_buffer = kmalloc(buffer_size);
    
    if (comp.back_buffer == NULL) {
        comp.back_buffer = framebuffer;  // Fallback
        kprintf("[COMP] Back buffer alloc failed!\n");
    }
    
    // Clear window pool
    for (int i = 0; i < 16; i++) {
        window_pool[i].active = 0;
    }
    
    kprintf("[COMP] Compositor initialized (%dx%d)\n", width, height);
}

comp_window_t *compositor_create_window(const char *title, int x, int y, int w, int h) {
    // Find free slot
    comp_window_t *win = NULL;
    for (int i = 0; i < 16; i++) {
        if (!window_pool[i].active) {
            win = &window_pool[i];
            break;
        }
    }
    if (!win) return NULL;
    
    win->id = next_window_id++;
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->active = 1;
    win->focused = 1;
    win->visible = 1;
    win->shadow = 1;
    win->rounded = 1;
    win->alpha = 230;  // Slightly transparent
    win->z_order = next_window_id;
    win->buffer = NULL;
    win->next = NULL;
    win->prev = NULL;
    strcopy(win->title, title, 64);
    
    // Add to window list
    if (comp.windows == NULL) {
        comp.windows = win;
    } else {
        comp_window_t *tail = comp.windows;
        while (tail->next) tail = tail->next;
        tail->next = win;
        win->prev = tail;
    }
    
    // Focus this window
    compositor_focus_window(win);
    comp.dirty = 1;
    
    kprintf("[COMP] Created window %d: %s\n", win->id, title);
    return win;
}

void compositor_focus_window(comp_window_t *win) {
    if (!win) return;
    
    // Unfocus all
    for (comp_window_t *w = comp.windows; w; w = w->next) {
        w->focused = 0;
    }
    win->focused = 1;
    comp.active = win;
    
    // Move to end of list (top of z-order)
    if (win->next) {
        // Remove from current position
        if (win->prev) win->prev->next = win->next;
        else comp.windows = win->next;
        win->next->prev = win->prev;
        
        // Add to tail
        comp_window_t *tail = comp.windows;
        while (tail->next) tail = tail->next;
        tail->next = win;
        win->prev = tail;
        win->next = NULL;
    }
    
    comp.dirty = 1;
}

void compositor_destroy_window(comp_window_t *win) {
    if (!win) return;
    
    // Remove from list
    if (win->prev) win->prev->next = win->next;
    else comp.windows = win->next;
    if (win->next) win->next->prev = win->prev;
    
    // Free buffer if allocated
    if (win->buffer) {
        kfree(win->buffer);
    }
    
    win->active = 0;
    comp.dirty = 1;
}

void compositor_invalidate(rect_t *region) {
    (void)region;  // For now, just redraw everything
    comp.dirty = 1;
}

void compositor_render(void) {
    static int last_mx = -1, last_my = -1;
    static uint8_t last_buttons = 0;
    
    int mx = mouse_get_x();
    int my = mouse_get_y();
    uint8_t buttons = mouse_get_buttons();
    
    // Handle input
    int left_pressed = (buttons & MOUSE_LEFT) && !(last_buttons & MOUSE_LEFT);
    
    // Window dragging state
    static comp_window_t *dragging = NULL;
    static int drag_ox = 0, drag_oy = 0;
    
    if (left_pressed) {
        // Check windows in reverse (top first)
        comp_window_t *hit = NULL;
        for (comp_window_t *w = comp.windows; w; w = w->next) {
            if (!w->active || !w->visible) continue;
            if (mx >= w->x && mx < w->x + w->width &&
                my >= w->y && my < w->y + w->height) {
                hit = w;  // Keep going to find topmost
            }
        }
        
        if (hit) {
            compositor_focus_window(hit);
            
            // Check if in title bar (not close button)
            if (my >= hit->y && my < hit->y + 28) {
                if (mx < hit->x + hit->width - 28) {
                    dragging = hit;
                    drag_ox = mx - hit->x;
                    drag_oy = my - hit->y;
                } else {
                    // Close button clicked
                    compositor_destroy_window(hit);
                }
            }
        }
    }
    
    if (buttons & MOUSE_LEFT) {
        if (dragging) {
            dragging->x = mx - drag_ox;
            dragging->y = my - drag_oy;
            if (dragging->x < 0) dragging->x = 0;
            if (dragging->y < 0) dragging->y = 0;
            comp.dirty = 1;
        }
    } else {
        dragging = NULL;
    }
    
    last_buttons = buttons;
    
    // Only redraw if something changed
    if (!comp.dirty && mx == last_mx && my == last_my) {
        return;
    }
    last_mx = mx;
    last_my = my;
    comp.dirty = 0;
    
    // Clear to desktop gradient
    for (int y = 0; y < comp.height; y++) {
        // Vertical gradient
        int r = 20 + (y * 15) / comp.height;
        int g = 40 + (y * 30) / comp.height;
        int b = 80 + (y * 40) / comp.height;
        uint32_t color = RGB(r, g, b);
        for (int x = 0; x < comp.width; x++) {
            comp.back_buffer[y * comp.width + x] = color;
        }
    }
    
    // Draw all windows
    for (comp_window_t *w = comp.windows; w; w = w->next) {
        draw_window(w);
    }
    
    // Draw dock/taskbar
    int dock_h = 48;
    int dock_w = 300;
    int dock_x = (comp.width - dock_w) / 2;
    int dock_y = comp.height - dock_h - 10;
    
    // Dock background with rounded corners and transparency
    draw_rounded_rect(dock_x, dock_y, dock_w, dock_h, 12, RGBA(30, 30, 30, 180));
    
    // Dock items
    draw_string_alpha(dock_x + 20, dock_y + 18, "RaeenOS", RGBA(100, 200, 255, 255));
    draw_string_alpha(dock_x + 120, dock_y + 18, "|", RGBA(100, 100, 100, 255));
    draw_string_alpha(dock_x + 140, dock_y + 18, "Shell", RGBA(200, 200, 200, 255));
    draw_string_alpha(dock_x + 200, dock_y + 18, "Info", RGBA(200, 200, 200, 255));
    
    // Draw cursor
    draw_cursor(mx, my);
    
    // Flip buffers
    if (comp.back_buffer != comp.front_buffer) {
        memcpy32(comp.front_buffer, comp.back_buffer, comp.width * comp.height);
    }
}

// Helper to draw text in window content area
void comp_draw_text(comp_window_t *win, int x, int y, const char *text, uint32_t color) {
    if (!win || !text) return;
    
    // If no buffer, allocate one
    if (!win->buffer) {
        size_t size = win->width * (win->height - 28) * sizeof(uint32_t);
        win->buffer = kmalloc(size);
        if (win->buffer) {
            memset32(win->buffer, 0, win->width * (win->height - 28));
        }
    }
    
    if (!win->buffer) return;
    
    // Draw to buffer
    while (*text) {
        if (*text >= 32 && *text <= 126) {
            int idx = *text - 32;
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = font_data[idx][row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        int px = x + col;
                        int py = y + row;
                        if (px >= 0 && px < win->width && py >= 0 && py < win->height - 28) {
                            win->buffer[py * win->width + px] = color;
                        }
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
    comp.dirty = 1;
}

// ========================================================================
// NEW: Window Management - Minimize/Maximize/Restore
// ========================================================================

void compositor_minimize_window(comp_window_t *win) {
    if (!win || !win->active) return;

    win->minimized = 1;
    win->visible = 0;
    comp.dirty = 1;

    kprintf("[COMP] Minimized window %d\n", win->id);
}

void compositor_maximize_window(comp_window_t *win) {
    if (!win || !win->active || win->maximized) return;

    // Save current position/size for restore
    win->restore_rect.x = win->x;
    win->restore_rect.y = win->y;
    win->restore_rect.w = win->width;
    win->restore_rect.h = win->height;

    // Maximize to full screen (minus taskbar)
    win->x = 0;
    win->y = 0;
    win->width = comp.width;
    win->height = comp.height - 60;  // Leave room for dock
    win->maximized = 1;
    win->minimized = 0;
    win->visible = 1;

    // Disable rounded corners and shadow when maximized
    win->rounded = 0;
    win->shadow = 0;

    comp.dirty = 1;
    kprintf("[COMP] Maximized window %d\n", win->id);
}

void compositor_restore_window(comp_window_t *win) {
    if (!win || !win->active) return;

    if (win->minimized) {
        // Restore from minimized
        win->minimized = 0;
        win->visible = 1;
    } else if (win->maximized) {
        // Restore from maximized
        win->x = win->restore_rect.x;
        win->y = win->restore_rect.y;
        win->width = win->restore_rect.w;
        win->height = win->restore_rect.h;
        win->maximized = 0;

        // Re-enable effects
        win->rounded = 1;
        win->shadow = 1;
    }

    comp.dirty = 1;
    kprintf("[COMP] Restored window %d\n", win->id);
}

void compositor_toggle_maximize(comp_window_t *win) {
    if (!win) return;

    if (win->maximized) {
        compositor_restore_window(win);
    } else {
        compositor_maximize_window(win);
    }
}

// ========================================================================
// NEW: Window Resizing
// ========================================================================

// Resize edge constants
#define RESIZE_NONE        0
#define RESIZE_TOP         1
#define RESIZE_BOTTOM      2
#define RESIZE_LEFT        3
#define RESIZE_RIGHT       4
#define RESIZE_TOP_LEFT    5
#define RESIZE_TOP_RIGHT   6
#define RESIZE_BOTTOM_LEFT 7
#define RESIZE_BOTTOM_RIGHT 8

#define RESIZE_BORDER 8  // Pixel width of resize border

int compositor_check_resize_edge(comp_window_t *win, int mouse_x, int mouse_y) {
    if (!win || !win->active || !win->visible || win->maximized) {
        return RESIZE_NONE;
    }

    int x = win->x;
    int y = win->y;
    int w = win->width;
    int h = win->height;

    // Check if mouse is near window edges
    int near_left = (mouse_x >= x - RESIZE_BORDER && mouse_x < x + RESIZE_BORDER);
    int near_right = (mouse_x >= x + w - RESIZE_BORDER && mouse_x < x + w + RESIZE_BORDER);
    int near_top = (mouse_y >= y - RESIZE_BORDER && mouse_y < y + RESIZE_BORDER);
    int near_bottom = (mouse_y >= y + h - RESIZE_BORDER && mouse_y < y + h + RESIZE_BORDER);

    // Check if mouse is actually over window area (not just near)
    int over_window = (mouse_x >= x && mouse_x < x + w && mouse_y >= y && mouse_y < y + h);

    // Corners have priority
    if (near_left && near_top) return RESIZE_TOP_LEFT;
    if (near_right && near_top) return RESIZE_TOP_RIGHT;
    if (near_left && near_bottom) return RESIZE_BOTTOM_LEFT;
    if (near_right && near_bottom) return RESIZE_BOTTOM_RIGHT;

    // Edges (only when over window)
    if (over_window && near_top && mouse_y < y + 28) return RESIZE_TOP;  // Not in title bar
    if (near_bottom) return RESIZE_BOTTOM;
    if (near_left) return RESIZE_LEFT;
    if (near_right) return RESIZE_RIGHT;

    return RESIZE_NONE;
}

void compositor_start_resize(comp_window_t *win, int edge) {
    if (!win) return;

    win->resizing = 1;
    win->resize_edge = edge;

    // Save original dimensions
    win->restore_rect.x = win->x;
    win->restore_rect.y = win->y;
    win->restore_rect.w = win->width;
    win->restore_rect.h = win->height;
}

void compositor_do_resize(comp_window_t *win, int mouse_x, int mouse_y) {
    if (!win || !win->resizing) return;

    int min_w = 200;
    int min_h = 100;

    int orig_x = win->restore_rect.x;
    int orig_y = win->restore_rect.y;
    int orig_w = win->restore_rect.w;
    int orig_h = win->restore_rect.h;

    switch (win->resize_edge) {
        case RESIZE_RIGHT:
            win->width = mouse_x - win->x;
            if (win->width < min_w) win->width = min_w;
            break;

        case RESIZE_BOTTOM:
            win->height = mouse_y - win->y;
            if (win->height < min_h) win->height = min_h;
            break;

        case RESIZE_LEFT:
            {
                int new_w = orig_x + orig_w - mouse_x;
                if (new_w >= min_w) {
                    win->x = mouse_x;
                    win->width = new_w;
                }
            }
            break;

        case RESIZE_TOP:
            {
                int new_h = orig_y + orig_h - mouse_y;
                if (new_h >= min_h) {
                    win->y = mouse_y;
                    win->height = new_h;
                }
            }
            break;

        case RESIZE_TOP_LEFT:
            {
                int new_w = orig_x + orig_w - mouse_x;
                int new_h = orig_y + orig_h - mouse_y;
                if (new_w >= min_w && new_h >= min_h) {
                    win->x = mouse_x;
                    win->y = mouse_y;
                    win->width = new_w;
                    win->height = new_h;
                }
            }
            break;

        case RESIZE_TOP_RIGHT:
            {
                int new_w = mouse_x - win->x;
                int new_h = orig_y + orig_h - mouse_y;
                if (new_w >= min_w && new_h >= min_h) {
                    win->y = mouse_y;
                    win->width = new_w;
                    win->height = new_h;
                }
            }
            break;

        case RESIZE_BOTTOM_LEFT:
            {
                int new_w = orig_x + orig_w - mouse_x;
                int new_h = mouse_y - win->y;
                if (new_w >= min_w && new_h >= min_h) {
                    win->x = mouse_x;
                    win->width = new_w;
                    win->height = new_h;
                }
            }
            break;

        case RESIZE_BOTTOM_RIGHT:
            win->width = mouse_x - win->x;
            win->height = mouse_y - win->y;
            if (win->width < min_w) win->width = min_w;
            if (win->height < min_h) win->height = min_h;
            break;
    }

    comp.dirty = 1;
}

void compositor_end_resize(comp_window_t *win) {
    if (!win) return;

    win->resizing = 0;
    win->resize_edge = RESIZE_NONE;

    // If window has a buffer, we need to reallocate it
    if (win->buffer) {
        kfree(win->buffer);
        win->buffer = NULL;

        size_t size = win->width * (win->height - 28) * sizeof(uint32_t);
        win->buffer = kmalloc(size);
        if (win->buffer) {
            memset32(win->buffer, 0, win->width * (win->height - 28));
        }
    }
}

// ========================================================================
// NEW: Window Snapping
// ========================================================================

void compositor_snap_window(comp_window_t *win, int snap_type) {
    if (!win || !win->active) return;

    int screen_w = comp.width;
    int screen_h = comp.height - 60;  // Leave room for dock

    // Save position before snapping (for unsnap)
    win->restore_rect.x = win->x;
    win->restore_rect.y = win->y;
    win->restore_rect.w = win->width;
    win->restore_rect.h = win->height;

    switch (snap_type) {
        case 0:  // Left half
            win->x = 0;
            win->y = 0;
            win->width = screen_w / 2;
            win->height = screen_h;
            break;

        case 1:  // Right half
            win->x = screen_w / 2;
            win->y = 0;
            win->width = screen_w / 2;
            win->height = screen_h;
            break;

        case 2:  // Top-left quarter
            win->x = 0;
            win->y = 0;
            win->width = screen_w / 2;
            win->height = screen_h / 2;
            break;

        case 3:  // Top-right quarter
            win->x = screen_w / 2;
            win->y = 0;
            win->width = screen_w / 2;
            win->height = screen_h / 2;
            break;

        case 4:  // Bottom-left quarter
            win->x = 0;
            win->y = screen_h / 2;
            win->width = screen_w / 2;
            win->height = screen_h / 2;
            break;

        case 5:  // Bottom-right quarter
            win->x = screen_w / 2;
            win->y = screen_h / 2;
            win->width = screen_w / 2;
            win->height = screen_h / 2;
            break;
    }

    win->rounded = 0;  // No rounded corners when snapped
    win->shadow = 0;
    win->maximized = 0;  // Snapped is different from maximized

    comp.dirty = 1;
    kprintf("[COMP] Snapped window %d to position %d\n", win->id, snap_type);
}

// ========================================================================
// NEW: Window Animations
// ========================================================================

void compositor_animate_window(comp_window_t *win, rect_t *target, int duration_ms) {
    if (!win || !target) return;

    win->animating = 1;
    win->anim_progress = 0;

    // Save current position as animation start
    win->anim_start.x = win->x;
    win->anim_start.y = win->y;
    win->anim_start.w = win->width;
    win->anim_start.h = win->height;

    // Save target
    win->anim_target = *target;

    // Duration is currently ignored (would need timer integration)
    (void)duration_ms;
}

void compositor_update_animations(void) {
    for (comp_window_t *w = comp.windows; w; w = w->next) {
        if (!w->animating) continue;

        // Progress from 0 to 255
        w->anim_progress += 32;  // ~8 frames for full animation

        if (w->anim_progress >= 255) {
            // Animation complete
            w->x = w->anim_target.x;
            w->y = w->anim_target.y;
            w->width = w->anim_target.w;
            w->height = w->anim_target.h;
            w->animating = 0;
            w->anim_progress = 255;
        } else {
            // Interpolate position
            int t = w->anim_progress;
            w->x = lerp(w->anim_start.x, w->anim_target.x, t);
            w->y = lerp(w->anim_start.y, w->anim_target.y, t);
            w->width = lerp(w->anim_start.w, w->anim_target.w, t);
            w->height = lerp(w->anim_start.h, w->anim_target.h, t);
        }

        comp.dirty = 1;
    }
}

// ========================================================================
// Helper: Fill rect in window buffer
// ========================================================================

void comp_fill_rect(comp_window_t *win, int x, int y, int w, int h, uint32_t color) {
    if (!win || !win->active) return;

    // Allocate buffer if needed
    if (!win->buffer) {
        size_t size = win->width * (win->height - 28) * sizeof(uint32_t);
        win->buffer = kmalloc(size);
        if (win->buffer) {
            memset32(win->buffer, 0, win->width * (win->height - 28));
        }
    }

    if (!win->buffer) return;

    // Draw filled rectangle
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < win->width && py >= 0 && py < win->height - 28) {
                win->buffer[py * win->width + px] = color;
            }
        }
    }

    comp.dirty = 1;
}
