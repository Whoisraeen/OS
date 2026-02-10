#include "gui.h"
#include "../u_stdlib.h"
#include "../font.h" // We assume font.h is available in include path

// Standard helpers if not in u_stdlib
static inline size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

static inline void strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for ( ; i < n; i++)
        dest[i] = '\0';
}

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
    comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
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
    if (x + w > win->width) w = win->width - x;
    if (y + h > win->height) h = win->height - y;
    
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
    if (c < 32 || c > 127) c = '?';
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
    int cx = x;
    while (*text) {
        gui_draw_char(win, cx, y, *text, color);
        cx += 8;
        text++;
    }
}

gui_window_t *gui_create_window(const char *title, int x, int y, int w, int h) {
    if (comp_port <= 0) gui_init();
    if (comp_port <= 0) return NULL;
    
    gui_window_t *win = (gui_window_t *)malloc(sizeof(gui_window_t));
    if (!win) return NULL;
    
    win->width = w;
    win->height = h;
    win->widgets = NULL;
    win->should_close = 0;
    
    // Create Shmem
    int buf_size = w * h * 4;
    win->shmem_id = syscall2(SYS_IPC_SHMEM_CREATE, buf_size, 0);
    win->buffer = (uint32_t *)syscall1(SYS_IPC_SHMEM_MAP, win->shmem_id);
    
    // Create Reply Port
    win->reply_port = syscall1(SYS_IPC_CREATE, IPC_PORT_FLAG_RECEIVE);
    
    // Clear Buffer (White)
    for (int i = 0; i < w * h; i++) win->buffer[i] = GUI_COLOR_WINDOW;
    
    // Send Create Message
    msg_create_window_t req;
    strncpy(req.title, title, 63);
    req.x = x; req.y = y; req.w = w; req.h = h;
    req.shmem_id = win->shmem_id;
    req.reply_port = win->reply_port;
    
    // We send raw bytes as data, but msg_id is handled by kernel?
    // Wait, our IPC syscall takes a pointer to struct.
    // The struct has msg_id.
    ipc_message_t msg;
    msg.msg_id = 1; // Create Window? Needs to match Compositor expectations.
    // Compositor checks msg.size == sizeof(msg_create_window_t).
    
    // We need to pack the req into msg.data
    // Or send req directly if syscall supports arbitrary buffer?
    // Syscall sends (msg, size).
    // The compositor expects a FULL ipc_message_t wrapper or just the payload?
    // Userspace compositor:
    // if (msg.size == sizeof(msg_create_window_t)) ...
    // So it expects the payload in msg.data.
    
    msg.size = sizeof(msg_create_window_t);
    // Copy req to msg.data
    char *d = (char *)msg.data;
    char *s = (char *)&req;
    for(size_t i=0; i<sizeof(msg_create_window_t); i++) d[i] = s[i];
    
    syscall3(SYS_IPC_SEND, comp_port, (long)&msg, 0); // Size 0 implies use internal size field?
    // Actually syscall implementation:
    // It copies the whole message struct.
    
    return win;
}

gui_widget_t *gui_create_button(gui_window_t *win, int x, int y, int w, int h, const char *text, gui_callback_t cb) {
    gui_widget_t *b = (gui_widget_t *)malloc(sizeof(gui_widget_t));
    b->type = 1; // Button
    b->x = x; b->y = y; b->width = w; b->height = h;
    strncpy(b->text, text, 63);
    b->on_event = cb;
    b->is_hovered = 0;
    b->parent = win;
    
    b->next = win->widgets;
    win->widgets = b;
    
    return b;
}

gui_widget_t *gui_create_label(gui_window_t *win, int x, int y, const char *text) {
    gui_widget_t *l = (gui_widget_t *)malloc(sizeof(gui_widget_t));
    l->type = 2; // Label
    l->x = x; l->y = y;
    strncpy(l->text, text, 63);
    l->width = strlen(text) * 8;
    l->height = 16;
    l->on_event = NULL;
    l->parent = win;
    
    l->next = win->widgets;
    win->widgets = l;
    
    return l;
}

static void render_widgets(gui_window_t *win) {
    // Clear
    for (int i = 0; i < win->width * win->height; i++) win->buffer[i] = GUI_COLOR_WINDOW;
    
    gui_widget_t *w = win->widgets;
    // We need to render in reverse order to draw first added last? Or list is stack?
    // List is LIFO (prepend). So last added is first in list.
    // Usually we want to draw back to front.
    // Let's just draw in list order (Top-most first? No, Top-most last).
    // Simple iteration for now.
    
    while (w) {
        if (w->type == 1) { // Button
            uint32_t bg = w->is_hovered ? GUI_COLOR_BUTTON_HOVER : GUI_COLOR_BUTTON;
            gui_draw_rect(win, w->x, w->y, w->width, w->height, bg);
            // Border
            gui_draw_rect(win, w->x, w->y, w->width, 1, GUI_COLOR_BORDER);
            gui_draw_rect(win, w->x, w->y + w->height - 1, w->width, 1, GUI_COLOR_BORDER);
            gui_draw_rect(win, w->x, w->y, 1, w->height, GUI_COLOR_BORDER);
            gui_draw_rect(win, w->x + w->width - 1, w->y, 1, w->height, GUI_COLOR_BORDER);
            
            // Text Centered
            int tw = strlen(w->text) * 8;
            int tx = w->x + (w->width - tw) / 2;
            int ty = w->y + (w->height - 16) / 2;
            gui_draw_text(win, tx, ty, w->text, GUI_COLOR_TEXT);
        } else if (w->type == 2) { // Label
            gui_draw_text(win, w->x, w->y, w->text, GUI_COLOR_TEXT);
        }
        w = w->next;
    }
}

void gui_main_loop(gui_window_t *win) {
    render_widgets(win);
    
    // Initial Paint
    ipc_message_t inv;
    inv.size = 0;
    syscall3(SYS_IPC_SEND, comp_port, (long)&inv, 0);
    
    ipc_message_t msg;
    while (!win->should_close) {
        long res = syscall3(SYS_IPC_RECV, win->reply_port, (long)&msg, 0);
        if (res == 0) {
            if (msg.size == sizeof(msg_input_event_t)) {
                msg_input_event_t *evt = (msg_input_event_t *)msg.data;
                
                int needs_redraw = 0;
                
                if (evt->type == 3) { // Mouse Move
                     // Check Hovers
                     gui_widget_t *w = win->widgets;
                     while (w) {
                         int hover = (evt->x >= w->x && evt->x < w->x + w->width &&
                                      evt->y >= w->y && evt->y < w->y + w->height);
                         if (hover != w->is_hovered) {
                             w->is_hovered = hover;
                             needs_redraw = 1;
                         }
                         w = w->next;
                     }
                }
                else if (evt->type == 4) { // Mouse Down
                    gui_widget_t *w = win->widgets;
                    while (w) {
                        if (evt->x >= w->x && evt->x < w->x + w->width &&
                            evt->y >= w->y && evt->y < w->y + w->height) {
                            
                            if (w->on_event) w->on_event(w, GUI_EVENT_CLICK, evt->x, evt->y);
                            needs_redraw = 1; // Assume click changes something
                            break; // Handle one widget
                        }
                        w = w->next;
                    }
                }
                
                if (needs_redraw) {
                    render_widgets(win);
                    inv.size = 0;
                    syscall3(SYS_IPC_SEND, comp_port, (long)&inv, 0);
                }
            }
        }
    }
}
