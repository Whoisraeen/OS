#include "lib/gui.h"
#include "u_stdlib.h"
#include "syscalls.h"

// Types missing in panel.c because they were private in gui.c
typedef struct {
    uint32_t msg_id;
    uint32_t sender_pid;
    uint32_t reply_port;
    uint32_t size;
    uint64_t timestamp;
    uint8_t data[128];
} ipc_message_t;

typedef struct {
    uint32_t type;
    uint32_t code;
    int32_t x;
    int32_t y;
} msg_input_event_t;

static long comp_port = 0;

static size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

// System Panel / Taskbar

static gui_window_t *win;
static gui_widget_t *clock_label;

void on_start_click(gui_widget_t *w, int event, int x, int y) {
    (void)w; (void)x; (void)y;
    if (event == GUI_EVENT_CLICK) {
        syscall1(SYS_PROC_EXEC, (long)"/initrd/terminal.elf");
    }
}

void on_shutdown_click(gui_widget_t *w, int event, int x, int y) {
    (void)w; (void)x; (void)y;
    if (event == GUI_EVENT_CLICK) {
        syscall1(SYS_SHUTDOWN, 0);
    }
}

static void update_clock() {
    // Get Time
    struct { uint64_t tv_sec; uint64_t tv_nsec; } ts;
    syscall1(SYS_CLOCK_GETTIME, (long)&ts);
    
    uint64_t t = ts.tv_sec;
    int sec = t % 60;
    int min = (t / 60) % 60;
    int hour = (t / 3600) % 24;
    
    char buf[16];
    
    buf[0] = '0' + (hour / 10);
    buf[1] = '0' + (hour % 10);
    buf[2] = ':';
    buf[3] = '0' + (min / 10);
    buf[4] = '0' + (min % 10);
    buf[5] = ':';
    buf[6] = '0' + (sec / 10);
    buf[7] = '0' + (sec % 10);
    buf[8] = 0;
    
    int changed = 0;
    for(int i=0; i<9; i++) {
        if (clock_label->text[i] != buf[i]) {
            clock_label->text[i] = buf[i];
            changed = 1;
        }
    }
}

void _start(void) {
    gui_init();
    
    // We need comp_port for invalidation in manual loop
    // But gui_init is in gui.c and comp_port is static there.
    // We should look it up ourselves or expose it.
    comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
    
    // Create Top Bar (Taskbar)
    // 1024x40 at top
    win = gui_create_window("Panel", 0, 0, 1024, 32);
    if (!win) syscall1(SYS_EXIT, 1);
    
    // Start Button
    gui_create_button(win, 4, 4, 60, 24, "Start", on_start_click);
    
    // Clock Label
    clock_label = gui_create_label(win, 1024 - 80, 8, "00:00:00");
    
    // Shutdown Button (Red accent?)
    gui_create_button(win, 1024 - 150, 4, 60, 24, "Power", on_shutdown_click);
    
    // Initial Render
    gui_draw_rect(win, 0, 0, 1024, 32, GUI_COLOR_BG);
    // Draw bottom border
    gui_draw_rect(win, 0, 31, 1024, 1, GUI_COLOR_BORDER);
    
    // Loop
    ipc_message_t msg;
    uint64_t last_tick = 0;
    
    for (;;) {
        long res = syscall3(SYS_IPC_RECV, win->reply_port, (long)&msg, 1);
        
        int redraw = 0;
        
        if (res == 0) {
             // Handle Input
             if (msg.size == sizeof(msg_input_event_t)) {
                msg_input_event_t *evt = (msg_input_event_t *)msg.data;
                
                if (evt->type == 3) { // Mouse Move
                     gui_widget_t *w = win->widgets;
                     while (w) {
                         int hover = (evt->x >= w->x && evt->x < w->x + w->width &&
                                      evt->y >= w->y && evt->y < w->y + w->height);
                         if (hover != w->is_hovered) {
                             w->is_hovered = hover;
                             redraw = 1;
                         }
                         w = w->next;
                     }
                }
                else if (evt->type == 4) { // Click
                    gui_widget_t *w = win->widgets;
                    while (w) {
                        if (evt->x >= w->x && evt->x < w->x + w->width &&
                            evt->y >= w->y && evt->y < w->y + w->height) {
                            if (w->on_event) w->on_event(w, GUI_EVENT_CLICK, evt->x, evt->y);
                            redraw = 1;
                        }
                        w = w->next;
                    }
                }
             }
        }
        
        // Update Clock
        struct { uint64_t tv_sec; uint64_t tv_nsec; } ts;
        syscall1(SYS_CLOCK_GETTIME, (long)&ts);
        if (ts.tv_sec != last_tick) {
            update_clock();
            last_tick = ts.tv_sec;
            redraw = 1;
        }
        
        if (redraw) {
            // Re-render all
             gui_draw_rect(win, 0, 0, 1024, 32, GUI_COLOR_BG);
             gui_draw_rect(win, 0, 31, 1024, 1, GUI_COLOR_BORDER);
             
             gui_widget_t *w = win->widgets;
             while (w) {
                 if (w->type == 1) { // Button
                    uint32_t bg = w->is_hovered ? GUI_COLOR_BUTTON_HOVER : GUI_COLOR_BUTTON;
                    gui_draw_rect(win, w->x, w->y, w->width, w->height, bg);
                    gui_draw_rect(win, w->x, w->y, w->width, 1, GUI_COLOR_BORDER);
                    gui_draw_rect(win, w->x, w->y + w->height - 1, w->width, 1, GUI_COLOR_BORDER);
                    gui_draw_rect(win, w->x, w->y, 1, w->height, GUI_COLOR_BORDER);
                    gui_draw_rect(win, w->x + w->width - 1, w->y, 1, w->height, GUI_COLOR_BORDER);
                    
                    int tw = strlen(w->text) * 8;
                    int tx = w->x + (w->width - tw) / 2;
                    int ty = w->y + (w->height - 16) / 2;
                    gui_draw_text(win, tx, ty, w->text, GUI_COLOR_TEXT);
                } else if (w->type == 2) { // Label
                    gui_draw_text(win, w->x, w->y, w->text, GUI_COLOR_TEXT);
                }
                w = w->next;
             }
             
             // Invalidate
             ipc_message_t inv;
             inv.size = 0;
             syscall3(SYS_IPC_SEND, comp_port, (long)&inv, 0);
        }
        
        syscall3(SYS_YIELD, 0, 0, 0);
    }
    
    syscall1(SYS_EXIT, 0);
}
