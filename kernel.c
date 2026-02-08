#include <stdint.h>
#include <stddef.h>
#include "limine/limine.h"
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
#include "cpu.h"
#include "heap.h"
#include "sched.h"
#include "timer.h"
#include "syscall.h"
#include "mouse.h"

// Globals for drivers to access
uint32_t *fb_ptr = NULL;
uint64_t fb_width = 0;
uint64_t fb_height = 0;

// =====================================================================
// Test tasks for verifying preemptive multitasking
// =====================================================================
void test_task1(void) {
    int x = 0;
    int y = 500;
    for (;;) {
        if (fb_ptr && (uint64_t)x < fb_width && (uint64_t)y < fb_height) {
             // Draw Red Line moving right
             fb_ptr[y * fb_width + x] = 0xFFFF0000;
        }
        x++;
        if (x >= 400) x = 0; // Wrap width
        
        // Busy wait
        for (volatile int i = 0; i < 1000000; i++);
        
        if (x == 0) {
            cpu_t *cpu = get_cpu();
            if (cpu) kprintf("[TASK1] Running on CPU %d\n", cpu->cpu_id);
        }
    }
}

void test_task2(void) {
    int x = 0;
    int y = 520;
    for (;;) {
        if (fb_ptr && (uint64_t)x < fb_width && (uint64_t)y < fb_height) {
             // Draw Green Line moving right
             fb_ptr[y * fb_width + x] = 0xFF00FF00;
        }
        x++;
        if (x >= 400) x = 0;
        
        // Busy wait
        for (volatile int i = 0; i < 1000000; i++);
        
        if (x == 0) {
            cpu_t *cpu = get_cpu();
            if (cpu) kprintf("[TASK2] Running on CPU %d\n", cpu->cpu_id);
        }
    }
}

void test_task3(void) {
    int sum = 0;
    for (;;) {
        sum += 1;
        if (sum % 1000000 == 0) {
            cpu_t *cpu = get_cpu();
            kprintf("[TASK3] Sum reached %d million on CPU %d\n", sum / 1000000, cpu ? cpu->cpu_id : 99);
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

    // Initialize GDT
    gdt_init();

    // Initialize Serial (for debug output)
    serial_init();
    kprintf("\n[KERNEL] GDT initialized.\n");

    // Initialize IDT
    idt_init();

    // Remap PIC
    pic_remap(32, 40);

    // Initialize Keyboard
    keyboard_init();

    // Initialize PMM
    pmm_init();

    // Initialize VMM (create our page tables)
    vmm_init();

    // Switch to our page tables
    vmm_switch();

    // Initialize Heap (needs PMM + VMM)
    heap_init();

    // Initialize SMP (Detect and wake up other cores)
    smp_init();

    // Initialize Timer (PIT for scheduling)
    timer_init();

    // Initialize Scheduler
    scheduler_init();

    // Enable Interrupts AFTER scheduler is ready to handle ticks
    __asm__ volatile ("sti");

    // Initialize IPC subsystem
    ipc_init();

    // Initialize security subsystem (capability-based)
    security_init();

    // Initialize syscalls (MSRs)
    syscall_init();

    // Draw the "Sony Blue" background
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
        
        // Launch init process
        vfs_node_t *init_node = initrd_find("init.elf");
        if (init_node) {
            console_printf("[KERNEL] Found init.elf, loading...\n");
            void *init_data = kmalloc(init_node->length);
            if (init_data) {
                vfs_read(init_node, 0, init_node->length, (uint8_t*)init_data);
                
                int pid = task_create_user("init", init_data, init_node->length);
                if (pid >= 0) {
                    console_printf("[KERNEL] Init process started (PID %d)\n", pid);
                } else {
                    console_printf("[KERNEL] Failed to start init process\n");
                }
                kfree(init_data);
            }
        } else {
            console_printf("[KERNEL] init.elf not found in initrd!\n");
        }
        
        // Compositor is now launched by init

    } else {
        console_printf(" Initrd: Not loaded (no module)\n");
    }
    
    // Initialize mouse
    mouse_init();
    
    // Main Loop (Kernel Idle)
    for (;;) {
        __asm__ volatile("hlt");
    }
}
