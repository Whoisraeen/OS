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

// static size_t strlen(const char *str) ...

// System Panel / Taskbar

static gui_window_t *win;
static gui_widget_t *clock_label;

void on_start_click(gui_widget_t *w, void *data) {
    (void)w; (void)data;
    // Launch Menu?
    // For now, spawn terminal
    syscall1(SYS_PROC_EXEC, (long)"terminal.elf");
}

void on_shutdown_click(gui_widget_t *w, void *data) {
    (void)w; (void)data;
    syscall0(SYS_SHUTDOWN);
}

static void update_clock() {
    // char buf[32];
    // uint64_t now = syscall0(SYS_CLOCK_GETTIME);
    // Format time...
    // strcpy(clock_label->text, buf);
}

void _start(void) {
    gui_init();
    
    // We need comp_port for invalidation in manual loop
    // But gui_init is in gui.c and comp_port is static there.
    // We should look it up ourselves or expose it.
    comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
    
    // Create Top Bar (Taskbar)
    // 1024x40 at top
    win = gui_create_window("Panel", 1024, 32);
    if (!win) syscall1(SYS_EXIT, 1);
    
    gui_window_add_child(win, (gui_widget_t*)gui_create_button(4, 4, 60, 24, "Start", on_start_click));
    
    clock_label = (gui_widget_t*)gui_create_label(1024 - 80, 8, "00:00:00");
    gui_window_add_child(win, clock_label);
    
    gui_window_add_child(win, (gui_widget_t*)gui_create_button(1024 - 150, 4, 60, 24, "Power", on_shutdown_click));
    
    gui_window_update(win);
    
    while (1) {
        if (!gui_window_process_events(win)) break;
        
        // Update Clock (every 1s)
        // uint64_t now = syscall0(SYS_CLOCK_GETTIME); // returns ticks/time?
        update_clock();
    }
}
