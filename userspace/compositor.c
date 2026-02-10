#include <stdint.h>
#include <stddef.h>
#include "font.h"
#include "gui.h"
#include "syscalls.h"
#include "u_stdlib.h"

// ============================================================================
// System Calls & IPC Definitions
// ============================================================================

// IPC Message Structure
typedef struct {
    uint32_t msg_id;
    uint32_t sender_pid;
    uint32_t reply_port;
    uint32_t size;
    uint64_t timestamp;
    uint8_t data[128];
} ipc_message_t;

// Standard library helpers
// Moved to u_stdlib.h


// ============================================================================
// Graphics & Compositor Structures
// ============================================================================

// Framebuffer Info
typedef struct {
    uint64_t addr;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint32_t bpp;
} fb_info_t;

static fb_info_t fb_info;
static uint32_t *fb_ptr;
static uint32_t *back_buffer;

// Color Macros
#define RGBA(r, g, b, a) ((((uint32_t)(a)) << 24) | (((uint32_t)(r)) << 16) | (((uint32_t)(g)) << 8) | ((uint32_t)(b)))
#define GET_A(c) (((c) >> 24) & 0xFF)
#define RGB(r, g, b) RGBA(r, g, b, 255)

// Theme
#define THEME_BG        RGBA(30, 58, 95, 255)
#define THEME_WINDOW    RGBA(32, 32, 32, 230)
#define THEME_TITLEBAR  RGBA(45, 45, 48, 255)
#define THEME_TITLE_FOC RGBA(0, 122, 204, 255)
#define THEME_SHADOW    RGBA(0, 0, 0, 80)

#define CORNER_RADIUS 8
#define SHADOW_OFFSET 6
#define SHADOW_SIZE   8

#define RESIZE_BORDER 8

#define RESIZE_NONE         0
#define RESIZE_TOP          1
#define RESIZE_BOTTOM       2
#define RESIZE_LEFT         3
#define RESIZE_RIGHT        4
#define RESIZE_TOP_LEFT     5
#define RESIZE_TOP_RIGHT    6
#define RESIZE_BOTTOM_LEFT  7
#define RESIZE_BOTTOM_RIGHT 8

// Window Structure
typedef struct comp_window {
    int id;
    int x, y;
    int width, height;
    char title[64];
    int active;
    int focused;
    int visible;
    int z_order;
    uint32_t shmem_id;
    uint32_t reply_port; // Added for input forwarding
    uint32_t *buffer;
    
    // Window Management
    int maximized;
    int saved_x, saved_y, saved_w, saved_h;
    
    // Animation
    int is_animating;
    int anim_start_x, anim_start_y, anim_start_w, anim_start_h;
    int anim_target_x, anim_target_y, anim_target_w, anim_target_h;
    uint64_t anim_start_time;
    uint64_t anim_duration;
    
    struct comp_window *next;
    struct comp_window *prev;
} comp_window_t;

#define MAX_WINDOWS 32
static comp_window_t window_pool[MAX_WINDOWS];
static comp_window_t *windows_list = NULL;
static int next_window_id = 1;
static int dirty = 1;

// Input Event
typedef struct {
    uint32_t type; // 1=Key, 2=Mouse
    uint32_t code; // Keycode or Buttons
    int32_t x;     // Mouse X
    int32_t y;     // Mouse Y
} input_event_t;

static int mouse_x = 0, mouse_y = 0;
static uint32_t mouse_buttons = 0;

static comp_window_t *resizing_window = NULL;
static int resize_edge = RESIZE_NONE;

static uint64_t get_time_ms() {
    struct { uint64_t tv_sec; uint64_t tv_nsec; } ts;
    syscall1(SYS_CLOCK_GETTIME, (uint64_t)&ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int check_resize_edge(comp_window_t *win, int mx, int my) {
    if (!win || !win->active || !win->visible || win->maximized) return RESIZE_NONE;
    
    int x = win->x;
    int y = win->y;
    int w = win->width;
    int h = win->height;
    
    int near_left = (mx >= x - RESIZE_BORDER && mx < x + RESIZE_BORDER);
    int near_right = (mx >= x + w - RESIZE_BORDER && mx < x + w + RESIZE_BORDER);
    int near_top = (my >= y - RESIZE_BORDER && my < y + RESIZE_BORDER);
    int near_bottom = (my >= y + h - RESIZE_BORDER && my < y + h + RESIZE_BORDER);
    int over_window = (mx >= x && mx < x + w && my >= y && my < y + h);
    
    if (near_left && near_top) return RESIZE_TOP_LEFT;
    if (near_right && near_top) return RESIZE_TOP_RIGHT;
    if (near_left && near_bottom) return RESIZE_BOTTOM_LEFT;
    if (near_right && near_bottom) return RESIZE_BOTTOM_RIGHT;
    
    if (over_window && near_top && my < y + 28) return RESIZE_TOP; 
    if (near_bottom) return RESIZE_BOTTOM;
    if (near_left) return RESIZE_LEFT;
    if (near_right) return RESIZE_RIGHT;
    
    return RESIZE_NONE;
}

static void snap_window(comp_window_t *win, int type) {
    if (!win) return;
    
    int target_x = win->x;
    int target_y = win->y;
    int target_w = win->width;
    int target_h = win->height;
    
    if (type == 1) { // Left
        target_x = 0;
        target_y = 0;
        target_w = fb_info.width / 2;
        target_h = fb_info.height;
    } else if (type == 2) { // Right
        target_x = fb_info.width / 2;
        target_y = 0;
        target_w = fb_info.width / 2;
        target_h = fb_info.height;
    } else if (type == 3) { // Maximize
        target_x = 0;
        target_y = 0;
        target_w = fb_info.width;
        target_h = fb_info.height;
    }
    
    // Start animation
    win->is_animating = 1;
    win->anim_start_x = win->x;
    win->anim_start_y = win->y;
    win->anim_start_w = win->width;
    win->anim_start_h = win->height;
    win->anim_target_x = target_x;
    win->anim_target_y = target_y;
    win->anim_target_w = target_w;
    win->anim_target_h = target_h;
    win->anim_start_time = get_time_ms();
    win->anim_duration = 200; // 200ms
}

static void update_animations() {
    uint64_t now = get_time_ms();
    
    for (comp_window_t *w = windows_list; w; w = w->next) {
        if (w->is_animating) {
            uint64_t elapsed = now - w->anim_start_time;
            if (elapsed >= w->anim_duration) {
                w->x = w->anim_target_x;
                w->y = w->anim_target_y;
                w->width = w->anim_target_w;
                w->height = w->anim_target_h;
                w->is_animating = 0;
                dirty = 1;
            } else {
                // Lerp
                int t = (elapsed * 1000) / w->anim_duration; // 0..1000
                w->x = w->anim_start_x + ((w->anim_target_x - w->anim_start_x) * t) / 1000;
                w->y = w->anim_start_y + ((w->anim_target_y - w->anim_start_y) * t) / 1000;
                w->width = w->anim_start_w + ((w->anim_target_w - w->anim_start_w) * t) / 1000;
                w->height = w->anim_start_h + ((w->anim_target_h - w->anim_start_h) * t) / 1000;
                dirty = 1;
            }
        }
    }
}

// Mouse State
static int last_mouse_x = 0, last_mouse_y = 0;
static uint32_t last_mouse_buttons = 0;
static comp_window_t *drag_window = NULL;
static int drag_off_x = 0, drag_off_y = 0;

static void destroy_window(comp_window_t *win) {
    if (!win) return;
    
    // Remove from list
    if (win->prev) win->prev->next = win->next;
    else windows_list = win->next;
    
    if (win->next) win->next->prev = win->prev;
    
    // Free shmem mapping if any
    if (win->shmem_id > 0) {
        syscall1(SYS_IPC_SHMEM_UNMAP, win->shmem_id);
    }
    
    win->active = 0;
    dirty = 1;
}

static void focus_window(comp_window_t *win) {
    if (!win) return;
    
    // Unfocus all
    for (comp_window_t *w = windows_list; w; w = w->next) {
        w->focused = 0;
    }
    win->focused = 1;
    
    // Move to end (top)
    if (win->next) {
        if (win->prev) win->prev->next = win->next;
        else windows_list = win->next;
        win->next->prev = win->prev;
        
        comp_window_t *tail = windows_list;
        while (tail->next) tail = tail->next;
        tail->next = win;
        win->prev = tail;
        win->next = NULL;
    }
    
    dirty = 1;
}

static comp_window_t *get_window_at(int px, int py) {
    // Check in reverse z-order (top first)
    comp_window_t *tail = windows_list;
    if (!tail) return NULL;
    while (tail->next) tail = tail->next;
    
    for (comp_window_t *w = tail; w; w = w->prev) {
        if (!w->visible) continue;
        if (px >= w->x && px < w->x + w->width &&
            py >= w->y && py < w->y + w->height) {
            return w;
        }
    }
    return NULL;
}

static void handle_key_event(uint32_t code) {
    // Send to focused window
    comp_window_t *focused = NULL;
    for (comp_window_t *w = windows_list; w; w = w->next) {
        if (w->focused) { focused = w; break; }
    }
    
    if (focused && focused->reply_port > 0) {
        ipc_message_t fwd;
        fwd.msg_id = MSG_INPUT_EVENT;
        fwd.size = sizeof(msg_input_event_t);
        msg_input_event_t *fe = (msg_input_event_t *)fwd.data;
        fe->type = EVENT_KEY_DOWN;
        fe->code = code;
        syscall3(SYS_IPC_SEND, focused->reply_port, (uint64_t)&fwd, 0);
    }
}

static void handle_mouse_event(int x, int y, uint32_t buttons) {
    mouse_x = x;
    mouse_y = y;
    mouse_buttons = buttons;
    
    int clicked = (mouse_buttons & 1) && !(last_mouse_buttons & 1);
    int released = !(mouse_buttons & 1) && (last_mouse_buttons & 1);
    
    if (clicked) {
        // 1. Find target (Resize edge or Window content)
        comp_window_t *target = NULL;
        int edge = RESIZE_NONE;
        
        // Iterate front to back (tail is top)
        comp_window_t *w = windows_list;
        while (w && w->next) w = w->next; 
        
        while (w) {
            edge = check_resize_edge(w, mouse_x, mouse_y);
            if (edge != RESIZE_NONE) {
                target = w;
                break;
            }
            // Check content hit
            if (mouse_x >= w->x && mouse_x < w->x + w->width &&
                mouse_y >= w->y && mouse_y < w->y + w->height) {
                target = w;
                break;
            }
            w = w->prev;
        }
        
        if (target) {
            focus_window(target);
            comp_window_t *hit = target;
            
            // Prioritize Resize over Drag if explicitly on edge
            // But Title Bar (Top) is tricky.
            // If RESIZE_TOP, check if we are in the "drag zone" vs "resize zone".
            // For now, if edge is detected, we resize.
            // Exception: If RESIZE_TOP and we are NOT at the very top pixel row?
            // Let's just use edge.
            
            if (edge != RESIZE_NONE && !hit->maximized) {
                resizing_window = target;
                resize_edge = edge;
            } else {
                // Title Bar / Content
                if (mouse_y >= hit->y && mouse_y < hit->y + 28) {
                     // Close Button
                     if (mouse_x >= hit->x + hit->width - 28 && mouse_x <= hit->x + hit->width - 8 &&
                        mouse_y >= hit->y + 4 && mouse_y <= hit->y + 24) {
                        destroy_window(hit);
                        goto update_state;
                    }
                    
                    // Restore if maximized and dragging
                    if (hit->maximized) {
                        hit->maximized = 0;
                        // Restore logic (simple centering)
                        hit->width = hit->saved_w ? hit->saved_w : 600;
                        hit->height = hit->saved_h ? hit->saved_h : 400;
                        hit->x = mouse_x - hit->width/2;
                        hit->y = mouse_y - 14;
                    }
                    
                    drag_window = hit;
                    drag_off_x = mouse_x - hit->x;
                    drag_off_y = mouse_y - hit->y;
                } else {
                    // Content Click
                    if (hit->reply_port > 0) {
                        ipc_message_t fwd;
                        fwd.msg_id = MSG_INPUT_EVENT;
                        fwd.size = sizeof(msg_input_event_t);
                        msg_input_event_t *fe = (msg_input_event_t *)fwd.data;
                        fe->type = EVENT_MOUSE_DOWN;
                        fe->code = 1; 
                        fe->x = mouse_x - hit->x;
                        fe->y = mouse_y - (hit->y + 28);
                        syscall3(SYS_IPC_SEND, hit->reply_port, (uint64_t)&fwd, 0);
                        dirty = 1;
                    }
                }
            }
        }
    }
    
    if (released) {
        if (drag_window) {
            // Snapping
            if (mouse_x <= 10) snap_window(drag_window, 1); // Left
            else if (mouse_x >= (int)fb_info.width - 10) snap_window(drag_window, 2); // Right
            else if (mouse_y <= 10) snap_window(drag_window, 3); // Maximize
        }
        drag_window = NULL;
        resizing_window = NULL;
        resize_edge = RESIZE_NONE;
    }
    
    if (mouse_buttons & 1) {
        if (resizing_window) {
             int dx = mouse_x - last_mouse_x;
             int dy = mouse_y - last_mouse_y;
             
             if (resize_edge == RESIZE_RIGHT || resize_edge == RESIZE_TOP_RIGHT || resize_edge == RESIZE_BOTTOM_RIGHT)
                 resizing_window->width += dx;
             if (resize_edge == RESIZE_LEFT || resize_edge == RESIZE_TOP_LEFT || resize_edge == RESIZE_BOTTOM_LEFT) {
                 resizing_window->x += dx;
                 resizing_window->width -= dx;
             }
             if (resize_edge == RESIZE_BOTTOM || resize_edge == RESIZE_BOTTOM_LEFT || resize_edge == RESIZE_BOTTOM_RIGHT)
                 resizing_window->height += dy;
             if (resize_edge == RESIZE_TOP || resize_edge == RESIZE_TOP_LEFT || resize_edge == RESIZE_TOP_RIGHT) {
                 resizing_window->y += dy;
                 resizing_window->height -= dy;
             }
             
             if (resizing_window->width < 100) resizing_window->width = 100;
             if (resizing_window->height < 100) resizing_window->height = 100;
             dirty = 1;
        } else if (drag_window) {
            drag_window->x = mouse_x - drag_off_x;
            drag_window->y = mouse_y - drag_off_y;
            dirty = 1;
        }
    }

update_state:
    last_mouse_buttons = mouse_buttons;
    if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) dirty = 1;
    last_mouse_x = mouse_x;
    last_mouse_y = mouse_y;
}


// ============================================================================
// Drawing Functions
// ============================================================================

static uint32_t blend_pixel(uint32_t fg, uint32_t bg) {
    uint32_t alpha = (fg >> 24) & 0xFF;
    if (alpha == 0) return bg;
    if (alpha == 255) return fg;
    
    uint32_t inv_alpha = 255 - alpha;
    uint32_t r = (((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * inv_alpha) / 255;
    uint32_t g = (((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * inv_alpha) / 255;
    uint32_t b = ((fg & 0xFF) * alpha + (bg & 0xFF) * inv_alpha) / 255;
    
    return RGBA(r, g, b, 255);
}

static void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || (uint64_t)x >= fb_info.width || y < 0 || (uint64_t)y >= fb_info.height) return;
    back_buffer[y * fb_info.width + x] = blend_pixel(color, back_buffer[y * fb_info.width + x]);
}

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            put_pixel(x + dx, y + dy, color);
        }
    }
}

// Check if pixel is inside rounded rect
static int inside_rounded_rect(int px, int py, int w, int h, int r) {
    if (px >= r && px < w - r) return 1;
    if (py >= r && py < h - r) return 1;
    
    int cx, cy;
    if (px < r && py < r) { cx = r; cy = r; }
    else if (px >= w - r && py < r) { cx = w - r - 1; cy = r; }
    else if (px < r && py >= h - r) { cx = r; cy = h - r - 1; }
    else { cx = w - r - 1; cy = h - r - 1; }
    
    int dx = px - cx;
    int dy = py - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

static void __attribute__((unused)) draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            if (inside_rounded_rect(dx, dy, w, h, r)) {
                put_pixel(x + dx, y + dy, color);
            }
        }
    }
}

static void draw_shadow(int x, int y, int w, int h) {
    for (int layer = SHADOW_SIZE; layer > 0; layer--) {
        int alpha = (80 * layer) / SHADOW_SIZE;
        uint32_t shadow = RGBA(0, 0, 0, alpha);
        int off = SHADOW_OFFSET + (SHADOW_SIZE - layer);
        
        // Draw edges
        for (int dx = 0; dx < w; dx++) put_pixel(x + dx + off, y + h + off - layer, shadow);
        for (int dy = 0; dy < h; dy++) put_pixel(x + w + off - layer, y + dy + off, shadow);
    }
    draw_rect(x + SHADOW_OFFSET, y + SHADOW_OFFSET, w, h, RGBA(0, 0, 0, 40));
}

static void draw_char(int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 126) return;
    int idx = c - 32;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font_data[idx][row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                put_pixel(x + col, y + row, color);
            }
        }
    }
}

static void draw_string(int x, int y, const char *str, uint32_t color) {
    while (*str) {
        draw_char(x, y, *str, color);
        x += FONT_WIDTH;
        str++;
    }
}

static void draw_cursor(int mx, int my) {
    static const uint8_t cursor[12] = {
        0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF, 0xFC, 0xFC, 0xCC, 0x06, 0x06
    };
    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 8; col++) {
            if (cursor[row] & (0x80 >> col)) {
                put_pixel(mx + col, my + row, 0xFFFFFFFF);
            }
        }
    }
}

// ============================================================================
// Window Management
// ============================================================================

static comp_window_t *create_window(const char *title, int x, int y, int w, int h, uint32_t shmem_id, uint32_t reply_port) {
    comp_window_t *win = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!window_pool[i].active) {
            win = &window_pool[i];
            break;
        }
    }
    if (!win) return NULL;
    
    win->id = next_window_id++;
    win->x = x; win->y = y; win->width = w; win->height = h;
    win->active = 1; win->visible = 1; win->focused = 1;
    win->reply_port = reply_port; // Store reply port
    strncpy(win->title, title, 63);
    
    // Map shared memory if provided
    win->shmem_id = shmem_id;
    win->buffer = NULL;
    if (shmem_id > 0) {
        long addr = syscall1(SYS_IPC_SHMEM_MAP, shmem_id);
        if (addr == -1) {
            win->buffer = NULL;
        } else {
            win->buffer = (uint32_t *)(uintptr_t)addr;
        }
    }
    
    // Add to list
    if (!windows_list) {
        windows_list = win;
        win->next = NULL;
        win->prev = NULL;
    } else {
        comp_window_t *tail = windows_list;
        while (tail->next) tail = tail->next;
        tail->next = win;
        win->prev = tail;
        win->next = NULL;
    }
    
    dirty = 1;
    return win;
}



static void draw_window(comp_window_t *win) {
    if (!win->visible) return;
    
    int x = win->x, y = win->y, w = win->width, h = win->height;
    
    // Shadow
    draw_shadow(x, y, w, h);
    
    // Title bar
    uint32_t title_color = win->focused ? THEME_TITLE_FOC : THEME_TITLEBAR;
    
    // Rounded top
    for (int dy = 0; dy < 28; dy++) {
        for (int dx = 0; dx < w; dx++) {
            if (inside_rounded_rect(dx, dy, w, 28 + CORNER_RADIUS, CORNER_RADIUS)) {
                put_pixel(x + dx, y + dy, title_color);
            }
        }
    }
    
    // Body (Draw window content or solid color)
    if (win->buffer) {
        for (int dy = 0; dy < h - 28; dy++) {
            for (int dx = 0; dx < w; dx++) {
                uint32_t pixel = win->buffer[dy * w + dx];
                // Simple alpha check
                if (GET_A(pixel) > 0) {
                    put_pixel(x + dx, y + 28 + dy, pixel);
                } else {
                    put_pixel(x + dx, y + 28 + dy, THEME_WINDOW);
                }
            }
        }
    } else {
        draw_rect(x, y + 28, w, h - 28, THEME_WINDOW);
    }
    
    // Title text
    draw_string(x + 10, y + 8, win->title, 0xFFFFFFFF);
    
    // Close button
    draw_rect(x + w - 28, y + 4, 20, 20, RGBA(200, 50, 50, 200));
    draw_string(x + w - 22, y + 8, "X", 0xFFFFFFFF);
}

// ============================================================================
// Main Compositor Loop
// ============================================================================

void _start(void) {
    // 1. Get Framebuffer
    if (syscall1(SYS_GET_FRAMEBUFFER, (uint64_t)&fb_info) != 0) {
        syscall1(SYS_EXIT, 1);
    }
    
    // Safety check to prevent division by zero
    if (fb_info.height == 0) fb_info.height = 1;
    if (fb_info.width == 0) fb_info.width = 1;

    fb_ptr = (uint32_t *)fb_info.addr;
    
    // Allocate Back Buffer (Double Buffering)
    size_t fb_size = fb_info.width * fb_info.height * 4;
    // We can try to use malloc if heap is large enough, or use SHMEM/mmap
    // Our malloc heap is 1MB by default in u_stdlib.h which is too small for 1024x768x4 (~3MB).
    // Let's create a private SHMEM region for backbuffer.
    long bb_shmem = syscall2(SYS_IPC_SHMEM_CREATE, fb_size, 0);
    if (bb_shmem > 0) {
        back_buffer = (uint32_t *)syscall1(SYS_IPC_SHMEM_MAP, bb_shmem);
    } else {
        // Fallback to front buffer (flickering)
        back_buffer = fb_ptr;
    }
    
    // 2. Setup IPC
    long port = syscall1(SYS_IPC_CREATE, IPC_PORT_FLAG_RECEIVE);
    if (port <= 0) {
        // Failed to create port
    } else {
        syscall3(SYS_IPC_REGISTER, port, (uint64_t)"compositor", 0);
    }
    
    // 3. Create initial window
    create_window("Welcome to RaeenOS", 100, 100, 400, 300, 0, 0);
    
    // 4. Main Loop
    ipc_message_t msg;
    input_event_t evt;
    
    for (;;) {
        // Handle IPC Messages (Non-blocking)
        long res = syscall3(SYS_IPC_RECV, port, (uint64_t)&msg, IPC_RECV_NONBLOCK);
        if (res == 0) {
            // Check for Window Creation Message
            // We expect the first word of data to be the Message Type if we are multiplexing
            // But msg_create_window_t doesn't have a type field at start...
            // Let's assume msg_id is overwritten by kernel, so we MUST use data payload.
            
            // Temporary Hack: Check message size to guess type
            // Create Window is sizeof(msg_create_window_t) = ~48 bytes
            // Input Event is sizeof(msg_input_event_t) = 16 bytes
            
            if (msg.size == sizeof(msg_create_window_t)) {
                msg_create_window_t *req = (msg_create_window_t *)msg.data;
                create_window(req->title, req->x, req->y, req->w, req->h, req->shmem_id, req->reply_port);
            }
            else if (msg.size == sizeof(msg_input_event_t)) {
                // Input Event from Driver
                msg_input_event_t *ievt = (msg_input_event_t *)msg.data;
                
                if (ievt->type == 1) { // Keyboard
                    handle_key_event(ievt->code);
                } else if (ievt->type == 3) { // Mouse Move
                    handle_mouse_event(ievt->x, ievt->y, ievt->code);
                }
            }
            else if (msg.size == 0) {
                // Force Redraw
                dirty = 1;
            }
        }
        
        // Handle Kernel Input (Legacy/Fallback)
        while (syscall1(SYS_GET_INPUT_EVENT, (uint64_t)&evt) == 1) {
            if (evt.type == 2) { // Mouse
                handle_mouse_event(evt.x, evt.y, evt.code);
            } else if (evt.type == 1) { // Keyboard
                handle_key_event(evt.code);
            }
        }
        
        // Update Animations
        update_animations();
        
        // Render if dirty
        if (dirty) {
            // Background
            for (uint64_t y = 0; y < fb_info.height; y++) {
                // Gradient
                int r = 20 + (y * 15) / fb_info.height;
                int g = 40 + (y * 30) / fb_info.height;
                int b = 80 + (y * 40) / fb_info.height;
                uint32_t color = RGB(r, g, b);
                for (uint64_t x = 0; x < fb_info.width; x++) {
                    back_buffer[y * fb_info.width + x] = color;
                }
            }
            
            // Windows
            for (comp_window_t *w = windows_list; w; w = w->next) {
                draw_window(w);
            }
            
            // Cursor
            draw_cursor(mouse_x, mouse_y);
            
            // Flip (Copy Back Buffer to Front Buffer)
            if (back_buffer != fb_ptr) {
                memcpy(fb_ptr, back_buffer, fb_info.width * fb_info.height * 4);
            } else {
                // Single buffering - nothing to flip
            }
            
            dirty = 0;
        }
        
        syscall3(SYS_YIELD, 0, 0, 0);
    }
    
    syscall1(SYS_EXIT, 0);
}
