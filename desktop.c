#include "desktop.h"
#include "mouse.h"
#include "font.h"
#include "serial.h"

// Framebuffer access
extern uint32_t *fb_ptr;
extern uint64_t fb_width;
extern uint64_t fb_height;

// Back buffer for double buffering
static uint32_t *back_buffer = NULL;

// Windows
static window_t windows[MAX_WINDOWS];

// Text lines for each window (simple storage)
#define MAX_TEXT_LINES 10
typedef struct {
    int x, y;
    char text[64];
    uint32_t color;
    int used;
} text_line_t;

static text_line_t window_text[MAX_WINDOWS][MAX_TEXT_LINES];

// Desktop colors
#define COLOR_DESKTOP    0xFF1E3A5F  // Dark blue
#define COLOR_TASKBAR    0xFF2D2D30  // Dark gray
#define COLOR_TITLEBAR   0xFF3C3C3C  // Window title
#define COLOR_TITLEBAR_F 0xFF007ACC  // Focused title (blue)
#define COLOR_WINDOW     0xFFFFFFFF  // Window background
#define COLOR_BORDER     0xFF555555  // Window border
#define COLOR_TEXT       0xFFFFFFFF  // White text
#define COLOR_CURSOR     0xFFFFFFFF  // Mouse cursor

// Drag state
static int dragging_window = -1;  // Which window is being dragged (-1 = none)
static int drag_offset_x = 0;     // Offset from window corner to mouse
static int drag_offset_y = 0;

// Start menu state
static int start_menu_open = 0;
#define MENU_X 0
#define MENU_Y (fb_height - 32 - 120)  // Above taskbar
#define MENU_W 150
#define MENU_H 120
#define MENU_ITEMS 4

// Draw a filled rectangle
static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < (int)fb_width && py >= 0 && py < (int)fb_height) {
                back_buffer[py * fb_width + px] = color;
            }
        }
    }
}

// Draw a character
static void draw_char(int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 126) return;
    int idx = c - 32;
    
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font_data[idx][row];  // 2D array access
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < (int)fb_width && py >= 0 && py < (int)fb_height) {
                    back_buffer[py * fb_width + px] = color;
                }
            }
        }
    }
}

// Draw a string
static void draw_string(int x, int y, const char *str, uint32_t color) {
    while (*str) {
        draw_char(x, y, *str++, color);
        x += FONT_WIDTH;
    }
}

// Draw mouse cursor (simple arrow)
static void draw_cursor(int mx, int my) {
    static const uint8_t cursor[12] = {
        0xC0,
        0xE0,
        0xF0,
        0xF8,
        0xFC,
        0xFE,
        0xFF,
        0xFC,
        0xFC,
        0xCC,
        0x06,
        0x06,
    };
    
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 8; x++) {
            if (cursor[y] & (0x80 >> x)) {
                int px = mx + x;
                int py = my + y;
                if (px >= 0 && px < (int)fb_width && py >= 0 && py < (int)fb_height) {
                    back_buffer[py * fb_width + px] = COLOR_CURSOR;
                }
            }
        }
    }
}

// Draw a window's content (text lines)
static void draw_window_content(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    
    window_t *win = &windows[id];
    if (!win->active) return;
    
    for (int i = 0; i < MAX_TEXT_LINES; i++) {
        if (window_text[id][i].used) {
            int wx = win->x + window_text[id][i].x;
            int wy = win->y + 20 + window_text[id][i].y;
            draw_string(wx, wy, window_text[id][i].text, window_text[id][i].color);
        }
    }
}

// Draw a window
static void draw_window(int id) {
    window_t *win = &windows[id];
    if (!win->active) return;
    
    int x = win->x;
    int y = win->y;
    int w = win->width;
    int h = win->height;
    
    // Border
    draw_rect(x - 1, y - 1, w + 2, h + 22, COLOR_BORDER);
    
    // Title bar
    uint32_t titlebar_color = win->focused ? COLOR_TITLEBAR_F : COLOR_TITLEBAR;
    draw_rect(x, y, w, 20, titlebar_color);
    
    // Title text
    draw_string(x + 4, y + 2, win->title, COLOR_TEXT);
    
    // Close button
    draw_rect(x + w - 18, y + 2, 16, 16, 0xFFE81123);
    draw_char(x + w - 14, y + 2, 'X', COLOR_TEXT);
    
    // Window content area
    draw_rect(x, y + 20, w, h, COLOR_WINDOW);
    
    // Draw stored text content
    draw_window_content(id);
}

void desktop_init(void) {
    // Allocate back buffer
    extern void *kmalloc(size_t size);
    back_buffer = (uint32_t *)kmalloc(fb_width * fb_height * sizeof(uint32_t));
    
    if (!back_buffer) {
        kprintf("[DESKTOP] Failed to allocate back buffer, using direct rendering\n");
        back_buffer = fb_ptr;
    }
    
    // Clear windows and text storage
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].active = 0;
        for (int j = 0; j < MAX_TEXT_LINES; j++) {
            window_text[i][j].used = 0;
        }
    }
    
    kprintf("[DESKTOP] Desktop initialized (%lux%lu)\n", fb_width, fb_height);
}

void desktop_render(void) {
    // Track last mouse state
    static int last_mx = -1, last_my = -1;
    static uint8_t last_buttons = 0;
    
    int mx = mouse_get_x();
    int my = mouse_get_y();
    uint8_t buttons = mouse_get_buttons();
    
    // Handle mouse input
    int left_pressed = (buttons & MOUSE_LEFT) && !(last_buttons & MOUSE_LEFT);
    int left_released = !(buttons & MOUSE_LEFT) && (last_buttons & MOUSE_LEFT);
    int left_held = (buttons & MOUSE_LEFT);
    
    // Start dragging if clicked on title bar
    if (left_pressed && dragging_window == -1) {
        // Check windows in reverse order (top window first)
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            if (!windows[i].active) continue;
            
            int wx = windows[i].x;
            int wy = windows[i].y;
            int ww = windows[i].width;
            
            // Check if click is in title bar (20 pixels high)
            if (mx >= wx && mx < wx + ww && my >= wy && my < wy + 20) {
                // Check if NOT on close button
                if (mx < wx + ww - 18) {
                    dragging_window = i;
                    drag_offset_x = mx - wx;
                    drag_offset_y = my - wy;
                    
                    // Focus this window
                    for (int k = 0; k < MAX_WINDOWS; k++) {
                        windows[k].focused = (k == i);
                    }
                }
                break;
            }
        }
    }
    
    // Update window position while dragging
    if (left_held && dragging_window >= 0) {
        windows[dragging_window].x = mx - drag_offset_x;
        windows[dragging_window].y = my - drag_offset_y;
        
        // Keep window on screen
        if (windows[dragging_window].x < 0) windows[dragging_window].x = 0;
        if (windows[dragging_window].y < 0) windows[dragging_window].y = 0;
    }
    
    // Stop dragging when button released
    if (left_released) {
        dragging_window = -1;
    }
    
    // Handle start menu toggle
    if (left_pressed) {
        // Check if clicked on start button (taskbar left side)
        if (mx >= 0 && mx < 70 && my >= (int)fb_height - 32 && my < (int)fb_height) {
            start_menu_open = !start_menu_open;
        }
        // Check if clicked on menu item when menu open
        else if (start_menu_open) {
            int menu_y = fb_height - 32 - MENU_H;
            if (mx >= MENU_X && mx < MENU_X + MENU_W &&
                my >= menu_y && my < menu_y + MENU_H) {
                // Which item was clicked?
                for (int i = 0; i < MENU_ITEMS; i++) {
                    int item_y = menu_y + 8 + i * 28;
                    if (my >= item_y && my < item_y + 24) {
                        start_menu_open = 0;  // Close menu
                        // Handle action
                        extern void speaker_click(void);
                        speaker_click();
                        if (i == 0) {
                            // Info - create info window
                            int w = window_create("System Info", 200, 150, 300, 150);
                            if (w >= 0) {
                                window_draw_text(w, 10, 20, "RaeenOS v0.1", 0xFF000000);
                                window_draw_text(w, 10, 40, "x86_64 Architecture", 0xFF000000);
                                window_draw_text(w, 10, 60, "GUI with mouse support", 0xFF000000);
                            }
                        } else if (i == 1) {
                            // Memory - create mem window
                            extern size_t heap_used(void);
                            extern size_t heap_free(void);
                            int w = window_create("Memory", 250, 200, 250, 100);
                            if (w >= 0) {
                                window_draw_text(w, 10, 20, "Memory Usage", 0xFF000000);
                                window_draw_text(w, 10, 40, "See 'mem' in shell", 0xFF666666);
                            }
                        } else if (i == 2) {
                            // Beep
                            extern void speaker_success(void);
                            speaker_success();
                        } else if (i == 3) {
                            // Reboot
                            __asm__ volatile ("lidt (0)\nint $0");
                        }
                        break;
                    }
                }
            } else {
                // Clicked outside menu - close it
                start_menu_open = 0;
            }
        }
    }
    
    // Update last state
    last_buttons = buttons;
    
    // Only redraw if mouse moved or button changed
    if (last_mx == mx && last_my == my && last_buttons == buttons && last_mx != -1) {
        return;  // No change, skip redraw
    }
    last_mx = mx;
    last_my = my;
    
    // Clear to desktop color
    for (size_t i = 0; i < fb_width * fb_height; i++) {
        back_buffer[i] = COLOR_DESKTOP;
    }
    
    // Draw taskbar at bottom
    draw_rect(0, fb_height - 32, fb_width, 32, COLOR_TASKBAR);
    
    // Start button (highlighted if menu open)
    if (start_menu_open) {
        draw_rect(0, fb_height - 32, 70, 32, 0xFF005A9E);  // Highlight
    }
    draw_string(8, fb_height - 24, "RaeenOS", 0xFF00AAFF);
    
    // Draw time/status on right side of taskbar
    draw_string(fb_width - 80, fb_height - 24, "10:15 AM", 0xFFCCCCCC);
    
    // Draw start menu if open
    if (start_menu_open) {
        int menu_y = fb_height - 32 - MENU_H;
        draw_rect(MENU_X, menu_y, MENU_W, MENU_H, 0xFF2D2D30);  // Menu bg
        draw_rect(MENU_X, menu_y, MENU_W, 2, 0xFF007ACC);       // Top accent
        
        // Menu items
        const char *items[] = {"Info", "Memory", "Beep", "Reboot"};
        for (int i = 0; i < MENU_ITEMS; i++) {
            int item_y = menu_y + 8 + i * 28;
            // Hover effect
            if (mx >= MENU_X && mx < MENU_X + MENU_W &&
                my >= item_y && my < item_y + 24) {
                draw_rect(MENU_X + 4, item_y, MENU_W - 8, 24, 0xFF3D3D40);
            }
            draw_string(MENU_X + 12, item_y + 6, items[i], 0xFFFFFFFF);
        }
    }
    
    // Draw windows (in order, so later windows are on top)
    for (int i = 0; i < MAX_WINDOWS; i++) {
        draw_window(i);
    }
    
    // Draw mouse cursor (always on top)
    draw_cursor(mx, my);
    
    // Copy back buffer to framebuffer (flip)
    if (back_buffer != fb_ptr) {
        for (size_t i = 0; i < fb_width * fb_height; i++) {
            fb_ptr[i] = back_buffer[i];
        }
    }
}

int window_create(const char *title, int x, int y, int width, int height) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) {
            windows[i].x = x;
            windows[i].y = y;
            windows[i].width = width;
            windows[i].height = height;
            windows[i].active = 1;
            windows[i].focused = 1;
            windows[i].buffer = NULL;
            
            // Copy title
            int j = 0;
            while (title[j] && j < 63) {
                windows[i].title[j] = title[j];
                j++;
            }
            windows[i].title[j] = '\0';
            
            // Clear text storage
            for (int k = 0; k < MAX_TEXT_LINES; k++) {
                window_text[i][k].used = 0;
            }
            
            // Unfocus other windows
            for (int k = 0; k < MAX_WINDOWS; k++) {
                if (k != i) windows[k].focused = 0;
            }
            
            kprintf("[DESKTOP] Created window %d: %s\n", i, title);
            return i;
        }
    }
    return -1;
}

void window_close(int id) {
    if (id >= 0 && id < MAX_WINDOWS) {
        windows[id].active = 0;
    }
}

void window_draw_text(int id, int x, int y, const char *text, uint32_t color) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    
    // Find a free text slot
    for (int i = 0; i < MAX_TEXT_LINES; i++) {
        if (!window_text[id][i].used) {
            window_text[id][i].x = x;
            window_text[id][i].y = y;
            window_text[id][i].color = color;
            window_text[id][i].used = 1;
            
            // Copy text
            int j = 0;
            while (text[j] && j < 63) {
                window_text[id][i].text[j] = text[j];
                j++;
            }
            window_text[id][i].text[j] = '\0';
            return;
        }
    }
}

void window_fill(int id, uint32_t color) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    draw_rect(windows[id].x, windows[id].y + 20, 
              windows[id].width, windows[id].height, color);
}
