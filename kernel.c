#include <stdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stddef.h>
#include "limine/limine.h"
#include "gdt.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "keyboard.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "console.h"
#include "vfs.h"
#include "initrd.h"
#include "user.h"
#include "ipc.h"
#include "security.h"

// Globals for drivers to access
uint32_t *fb_ptr = NULL;
uint64_t fb_width = 0;
uint64_t fb_height = 0;

// =====================================================================
// Test tasks for verifying preemptive multitasking
// =====================================================================
void test_task1(void) {
    int count = 0;
    for (;;) {
        kprintf("[TASK1] Running (count=%d)\n", count++);
        // Busy wait to make output visible
        for (volatile int i = 0; i < 5000000; i++);
    }
}

void test_task2(void) {
    int count = 0;
    for (;;) {
        kprintf("[TASK2] Alive! (count=%d)\n", count++);
        // Busy wait
        for (volatile int i = 0; i < 5000000; i++);
    }
}

void test_task3(void) {
    int sum = 0;
    for (;;) {
        sum += 1;
        if (sum % 1000000 == 0) {
            kprintf("[TASK3] Sum reached %d million\n", sum / 1000000);
        }
    }
}

// Set the base revision to 3, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimize them away, so, usually, they should
// be made volatile or equivalent.
__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

// Request modules (initrd)
__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

// The following will be our kernel's entry point.
void _start(void) {
    // Ensure we got a framebuffer.
    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        // If not, just halt.
        for (;;) {
            __asm__("hlt");
        }
    }

    // Fetch the first framebuffer.
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];

    // Note: we assume the framebuffer model is RGB with 32-bit pixels.
    fb_ptr = (uint32_t *)framebuffer->address;
    fb_width = framebuffer->width;
    fb_height = framebuffer->height;

    // DEBUG: Clear to BLUE immediately (Check 1: Bootloader passed control)
    for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFF0000FF; // Blue

    // Initialize GDT
    gdt_init();
    
    // Initialize Serial (for debug output)
    serial_init();
    kprintf("\n[KERNEL] GDT initialized.\n");
    
    // DEBUG: Clear to GREEN (Check 2: GDT OK)
    for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFF00FF00; // Green

    // Initialize IDT
    idt_init();
    
    // Remap PIC
    pic_remap(32, 40);

    // Initialize Keyboard
    keyboard_init();
    
    // DEBUG: Clear to CYAN (Check 3: Interrupts Setup OK)
    for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFF00FFFF; // Cyan

    // Enable Interrupts
    __asm__ volatile ("sti");

    // Initialize PMM
    pmm_init();
    
    // Initialize VMM (create our page tables)
    vmm_init();
    
    // Switch to our page tables
    vmm_switch();
    
    // Initialize Timer (PIT for scheduling)
    extern void timer_init(void);
    timer_init();

    // Initialize Scheduler
    extern void scheduler_init(void);
    scheduler_init();

    // Initialize IPC subsystem
    ipc_init();

    // Initialize security subsystem (capability-based)
    security_init();

    // =====================================================================
    // TEST: Create test tasks to verify preemptive multitasking
    // =====================================================================
    kprintf("\n[KERNEL] Creating test tasks for multitasking...\n");

    // Create the test tasks (functions defined at top of file)
    extern int task_create(const char *name, void (*entry)(void));
    task_create("test_task1", test_task1);
    task_create("test_task2", test_task2);
    task_create("test_task3", test_task3);

    // Print task list to verify they were created
    extern void scheduler_debug_print_tasks(void);
    scheduler_debug_print_tasks();

    kprintf("\n[KERNEL] Multitasking should now be active!\n");
    kprintf("[KERNEL] You should see interleaved output from tasks 1, 2, and 3.\n\n");

    // Let tasks run for a bit to verify scheduling works
    extern void timer_sleep(uint32_t ms);
    timer_sleep(5000);  // Let tasks run for 5 seconds

    kprintf("\n[KERNEL] Multitasking test complete! Continuing with compositor...\n\n");
    // =====================================================================
    
    // DEBUG: Clear to WHITE (Check 4: VMM Switch OK)
    for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFFFFFFFF; // White

    // Draw the "Sony Blue" background for the final state
    for (size_t i = 0; i < fb_width * fb_height; i++) {
        fb_ptr[i] = 0xFF003366; 
    }

    // Initialize graphical console
    console_init();
    console_printf("\n");
    console_printf("  ____                   ___  ____  \n");
    console_printf(" |  _ \\ __ _  ___  ___ _ __ / _ \\/ ___| \n");
    console_printf(" | |_) / _` |/ _ \\/ _ \\ '_ \\ | | \\___ \\ \n");
    console_printf(" |  _ < (_| |  __/  __/ | | | |_| |___) |\n");
    console_printf(" |_| \\_\\__,_|\\___|\\___|_| |_|\\___/|____/ \n");
    console_printf("\n");
    console_printf(" Welcome to RaeenOS!\n");
    console_printf(" ----------------------------------\n");
    console_printf(" GDT: OK | IDT: OK | PIC: OK\n");
    console_printf(" PMM: OK | VMM: OK | Heap: OK\n");
    console_printf(" Serial: COM1 @ 38400 baud\n");
    console_printf(" Framebuffer: %lux%lu @ 32bpp\n", fb_width, fb_height);
    console_printf("\n");
    
    // Initialize initrd from Limine module
    if (module_request.response != NULL && module_request.response->module_count > 0) {
        struct limine_file *initrd_file = module_request.response->modules[0];
        console_printf(" Initrd: %s (%lu bytes)\n", initrd_file->path, initrd_file->size);
        
        vfs_root = initrd_init(initrd_file->address, initrd_file->size);
        console_printf(" VFS: Mounted initrd at /\n");
        
        // List files in initrd
        console_printf("\n Files in initrd:\n");
        for (size_t i = 0; ; i++) {
            vfs_node_t *node = vfs_readdir(vfs_root, i);
            if (node == NULL) break;
            console_printf("   - %s (%lu bytes)\n", node->name, node->length);
        }
        
        // Try to read hello.txt
        vfs_node_t *hello = initrd_find("hello.txt");
        if (hello != NULL) {
            console_printf("\n Contents of hello.txt:\n   ");
            uint8_t buffer[256];
            size_t read = vfs_read(hello, 0, 255, buffer);
            buffer[read] = '\0';
            console_printf("%s\n", (char *)buffer);
        }
    } else {
        console_printf(" Initrd: Not loaded (no module)\n");
    }
    
    // Initialize mouse
    extern void mouse_init(void);
    mouse_init();
    
    // Initialize compositor (next-gen window manager)
    extern void compositor_init(uint32_t *framebuffer, int width, int height);
    extern void compositor_render(void);
    extern void *compositor_create_window(const char *title, int x, int y, int w, int h);
    extern void comp_draw_text(void *win, int x, int y, const char *text, uint32_t color);
    
    compositor_init(fb_ptr, fb_width, fb_height);
    
    // Initialize Security and IPC
    extern void security_init(void);
    extern void ipc_init(void);
    security_init();  // Must be first (defines capabilities)
    ipc_init();       // Uses security checks

    // Initialize syscalls (MSRs)
    extern void syscall_init(void);
    syscall_init();

    // Create demo windows with new compositor
    void *win1 = compositor_create_window("Welcome to RaeenOS!", 100, 80, 420, 220);
    if (win1) {
        comp_draw_text(win1, 10, 10, "RaeenOS - A hobby operating system", 0xFFFFFFFF);
        comp_draw_text(win1, 10, 35, "Features:", 0xFFCCCCCC);
        comp_draw_text(win1, 10, 55, "* Alpha blending & transparency", 0xFF88CCFF);
        comp_draw_text(win1, 10, 75, "* Drop shadows & rounded corners", 0xFF88CCFF);
        comp_draw_text(win1, 10, 95, "* Compositing window manager", 0xFF88CCFF);
        comp_draw_text(win1, 10, 115, "* Centered macOS-style dock", 0xFF88CCFF);
        comp_draw_text(win1, 10, 140, "Drag windows by title bar!", 0xFFFFCC00);
    }
    
    void *win2 = compositor_create_window("System Info", 580, 120, 320, 180);
    if (win2) {
        comp_draw_text(win2, 10, 10, "Architecture: x86_64", 0xFFFFFFFF);
        comp_draw_text(win2, 10, 30, "Timer: 100 Hz PIT", 0xFFCCCCCC);
        comp_draw_text(win2, 10, 50, "Mouse: PS/2 with IRQ12", 0xFFCCCCCC);
        comp_draw_text(win2, 10, 70, "Heap: 4MB kernel heap", 0xFFCCCCCC);
        comp_draw_text(win2, 10, 100, "Next-Gen Compositor", 0xFF00FF88);
    }
    
    console_printf("\n Compositor started!\n");
    console_printf(" Drag windows, click X to close!\n");
    
    // Main compositor loop
    for (;;) {
        compositor_render();
        // Small delay
        for (volatile int i = 0; i < 50000; i++);
    }
}
