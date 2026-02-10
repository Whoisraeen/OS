#include <stdint.h>
#include <stddef.h>
// #include <string.h> // Do not include standard string.h if we don't link with libc
#include "gui.h"
#include "../u_stdlib.h"
#include "../font.h" // We assume font.h is available in include path

// Utils moved to u_stdlib.h

// IPC Structs
typedef struct {
    uint32_t msg_id;
    uint32_t sender_pid;
    uint32_t reply_port;
    uint32_t size;
    uint64_t timestamp;
    uint8_t data[128];
} ipc_message_t;

typedef struct {
    char title[64];
    int x, y;
    int w, h;
    uint32_t shmem_id;
    uint32_t reply_port;
} msg_create_window_t;

typedef struct {
    uint32_t type;
    uint32_t code;
    int32_t x;
    int32_t y;
} msg_input_event_t;

// Global State
static long comp_port = 0;

void gui_init(void) {
    while (comp_port <= 0) {
        comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
        if (comp_port <= 0) {
             // Yield and retry
             syscall1(SYS_YIELD, 0);
             for(volatile int i=0; i<1000000; i++);
        }
    }
}

static uint32_t blend(uint32_t fg, uint32_t bg) {
    uint32_t alpha = (fg >> 24) & 0xFF;
    if (alpha == 0) return bg;
    if (alpha == 255) return fg;
    
    uint32_t inv_alpha = 255 - alpha;
    uint32_t r = (((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * inv_alpha) / 255;
    uint32_t g = (((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * inv_alpha) / 255;
    uint32_t b = ((fg & 0xFF) * alpha + (bg & 0xFF) * inv_alpha) / 255;
    
    return RGBA(r, g, b, 255);
}

void gui_draw_rect(gui_window_t *win, int x, int y, int w, int h, uint32_t color) {
    if (!win || !win->buffer) return;
    
    // Clip
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if ((int)(x + w) > (int)win->width) w = win->width - x;
    if ((int)(y + h) > (int)win->height) h = win->height - y;
    
    if (w <= 0 || h <= 0) return;
    
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int off = (y + dy) * win->width + (x + dx);
            win->buffer[off] = blend(color, win->buffer[off]);
        }
    }
}

// Basic Font Drawing (copied logic from compositor/terminal)
// Assuming FONT_WIDTH=8, FONT_HEIGHT=16 and font_bitmap available
// We need to link font.o or include font_data.
// For now, let's implement a dummy or simple one if font.h doesn't provide data.
// Wait, font.h usually has the extern. We need font.c to be linked.

extern const uint8_t font_bitmap[]; 
#define FONT_WIDTH 8
#define FONT_HEIGHT 16

void gui_draw_char(gui_window_t *win, int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = &font_bitmap[(c - 32) * 16];
    
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << (7 - col))) {
                gui_draw_rect(win, x + col, y + row, 1, 1, color);
            }
        }
    }
}

void gui_draw_text(gui_window_t *win, int x, int y, const char *text, uint32_t color) {
    int cur_x = x;
    while (*text) {
        gui_draw_char(win, cur_x, y, *text, color);
        cur_x += FONT_WIDTH;
        text++;
    }
}

// ============================================================================
// Widget System Implementation
// ============================================================================

static void gui_draw_widget(gui_window_t *win, gui_widget_t *w);

gui_window_t *gui_create_window(const char *title, int width, int height) {
    if (comp_port == 0) gui_init();
    
    gui_window_t *win = malloc(sizeof(gui_window_t));
    if (!win) return NULL;
    
    memset(win, 0, sizeof(gui_window_t));
    win->base.type = WIDGET_WINDOW;
    win->base.w = width;
    win->base.h = height;
    win->width = width;
    win->height = height;
    win->bg_color = COLOR_WHITE;
    strncpy(win->title, title, 63);
    
    // Create Shmem
    size_t size = width * height * 4;
    uint32_t shmem_id = syscall2(SYS_IPC_SHMEM_CREATE, size, 1); // 1=Writable
    if ((int)shmem_id < 0) {
        free(win);
        return NULL;
    }
    
    win->shmem_id = shmem_id;
    win->buffer = (uint32_t *)syscall2(SYS_IPC_SHMEM_MAP, shmem_id, 0);
    if ((long)win->buffer == -1) {
        // Retry logic for shmem mapping?
        // For now, fail hard to debug
        // printf("GUI: Failed to map shmem %d\n", shmem_id);
        free(win);
        return NULL;
    }
    
    // Clear Buffer
    for (size_t i = 0; i < (size_t)(width * height); i++) win->buffer[i] = win->bg_color;
    
    // Create Reply Port
    int reply_port = syscall1(SYS_IPC_CREATE, 0);
    win->event_port = reply_port;
    
    // Send Create Message to Compositor
    msg_create_window_t req;
    strncpy(req.title, title, 63);
    req.x = 100; // Default pos
    req.y = 100;
    req.w = width;
    req.h = height;
    req.shmem_id = shmem_id;
    req.reply_port = reply_port;
    
    ipc_message_t msg;
    msg.msg_id = 100; // CREATE_WINDOW
    msg.reply_port = reply_port;
    msg.size = sizeof(req);
    memcpy(msg.data, &req, sizeof(req));
    
    syscall3(SYS_IPC_SEND, comp_port, (long)&msg, 0);
    
    return win;
}

void gui_window_add_child(gui_window_t *win, gui_widget_t *child) {
    if (!win || !child) return;
    
    child->parent = (gui_widget_t *)win;
    
    if (!win->base.first_child) {
        win->base.first_child = child;
    } else {
        gui_widget_t *last = win->base.first_child;
        while (last->next_sibling) last = last->next_sibling;
        last->next_sibling = child;
    }
}

static void gui_draw_button(gui_window_t *win, gui_button_t *btn) {
    uint32_t color = btn->is_hovered ? btn->hover_color : btn->color;
    if (btn->is_pressed) color = blend(COLOR_BLACK, color); // Darken
    
    gui_draw_rect(win, btn->base.x, btn->base.y, btn->base.w, btn->base.h, color);
    
    // Center Text (Roughly)
    int text_len = strlen(btn->text);
    int tx = btn->base.x + (btn->base.w - text_len * FONT_WIDTH) / 2;
    int ty = btn->base.y + (btn->base.h - FONT_HEIGHT) / 2;
    
    gui_draw_text(win, tx, ty, btn->text, btn->text_color);
}

static void gui_draw_label(gui_window_t *win, gui_label_t *lbl) {
    gui_draw_text(win, lbl->base.x, lbl->base.y, lbl->text, lbl->text_color);
}

static void gui_draw_widget(gui_window_t *win, gui_widget_t *w) {
    if (!w) return;
    
    if (w->type == WIDGET_BUTTON) {
        gui_draw_button(win, (gui_button_t*)w);
    } else if (w->type == WIDGET_LABEL) {
        gui_draw_label(win, (gui_label_t*)w);
    }
    
    // Draw children
    gui_widget_t *child = w->first_child;
    while (child) {
        gui_draw_widget(win, child);
        child = child->next_sibling;
    }
}

void gui_window_update(gui_window_t *win) {
    // Redraw Background
    gui_draw_rect(win, 0, 0, win->width, win->height, win->bg_color);
    
    // Draw Widgets
    gui_widget_t *child = win->base.first_child;
    while (child) {
        gui_draw_widget(win, child);
        child = child->next_sibling;
    }
    
    // Notify Compositor (Damage)
    // Simplified: Just say "updated"
    ipc_message_t msg;
    msg.msg_id = 102; // WINDOW_UPDATE
    msg.reply_port = win->event_port;
    msg.size = 0;
    syscall3(SYS_IPC_SEND, comp_port, (long)&msg, 0);
}

// Event Dispatch
static int is_point_in_rect(int px, int py, int x, int y, int w, int h) {
    return (px >= x && px < x + w && py >= y && py < y + h);
}

static void gui_handle_mouse(gui_window_t *win, int type, int x, int y) {
    // Traverse widgets to find target
    gui_widget_t *w = win->base.first_child;
    while (w) {
        if (is_point_in_rect(x, y, w->x, w->y, w->w, w->h)) {
            if (w->type == WIDGET_BUTTON) {
                gui_button_t *btn = (gui_button_t*)w;
                if (type == 2) { // Mouse Move
                    if (!btn->is_hovered) {
                        btn->is_hovered = 1;
                        gui_window_update(win);
                    }
                } else if (type == 1) { // Click
                    btn->is_pressed = 1;
                    gui_window_update(win);
                    if (w->on_click) w->on_click(w, NULL);
                    // Reset press after short delay or on release?
                    // For now, simple immediate reset on next update logic or just leave it
                    btn->is_pressed = 0; 
                    gui_window_update(win);
                }
            }
        } else {
             if (w->type == WIDGET_BUTTON) {
                gui_button_t *btn = (gui_button_t*)w;
                if (btn->is_hovered) {
                    btn->is_hovered = 0;
                    gui_window_update(win);
                }
             }
        }
        w = w->next_sibling;
    }
}

int gui_window_process_events(gui_window_t *win) {
    ipc_message_t msg;
    // Non-blocking check? Or blocking?
    // Let's do non-blocking if possible, or blocking if this is the main loop
    // For now, blocking receive
    long res = syscall3(SYS_IPC_RECV, win->event_port, (long)&msg, sizeof(msg));
    if (res < 0) return 1; // Error or no message
    
    if (msg.msg_id == 200) { // INPUT_EVENT
        msg_input_event_t *evt = (msg_input_event_t*)msg.data;
        if (evt->type == 2) { // Mouse
            // Adjust coords relative to window? 
            // Compositor sends relative coords usually.
            gui_handle_mouse(win, 2, evt->x, evt->y); // Move
        } else if (evt->type == 1) { // Key
             // Handle key
        } else if (evt->type == 3) { // Mouse Click
            gui_handle_mouse(win, 1, evt->x, evt->y);
        }
    } else if (msg.msg_id == 999) { // CLOSE
        return 0;
    }
    
    return 1;
}

// Factory Functions
gui_button_t *gui_create_button(int x, int y, int w, int h, const char *text, event_handler_t on_click) {
    gui_button_t *btn = malloc(sizeof(gui_button_t));
    memset(btn, 0, sizeof(gui_button_t));
    btn->base.type = WIDGET_BUTTON;
    btn->base.x = x;
    btn->base.y = y;
    btn->base.w = w;
    btn->base.h = h;
    btn->base.on_click = on_click;
    strncpy(btn->text, text, 31);
    btn->color = COLOR_GRAY;
    btn->hover_color = RGBA(150, 150, 150, 255);
    btn->text_color = COLOR_BLACK;
    return btn;
}

gui_label_t *gui_create_label(int x, int y, const char *text) {
    gui_label_t *lbl = malloc(sizeof(gui_label_t));
    memset(lbl, 0, sizeof(gui_label_t));
    lbl->base.type = WIDGET_LABEL;
    lbl->base.x = x;
    lbl->base.y = y;
    strncpy(lbl->text, text, 63);
    lbl->text_color = COLOR_BLACK;
    return lbl;
}
