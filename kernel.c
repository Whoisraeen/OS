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
#include "acpi.h"
#include "ioapic.h"
#include "rtc.h"
#include "lapic.h"
#include "driver.h"
#include "pci.h"
#include "devfs.h"
#include "ahci.h"
#include "drivers/e1000.h"
#include "drivers/hda.h"
#include "block.h"
#include "partition.h"
#include "bcache.h"
#include "ext2.h"
#include "bga.h"
#include "string.h"
#include "klog.h"
#include "ksyms.h"
#include "elf.h"
#include "aio.h"

// Globals for drivers to access
uint32_t *fb_ptr = NULL;
uint64_t fb_width = 0;
uint64_t fb_height = 0;

static vfs_node_t *initrd_root_node = NULL;

static void background_installer_task(void) {
    if (!initrd_root_node || !ext2_root_fs) {
        kprintf("[INSTALL] Background task: Missing initrd or disk!\n");
        task_exit();
        return;
    }

    kprintf("[INSTALL] Background Installation Started...\n");

    // Re-scan initrd
    for (size_t i = 0; ; i++) {
        vfs_node_t *node = vfs_readdir(initrd_root_node, i);
        if (node == NULL) break;

        // Skip . and disk
        if (strcmp(node->name, ".") == 0 || strcmp(node->name, "..") == 0 || strcmp(node->name, "disk") == 0) {
            continue;
        }
        
        // Skip critical files already installed
        if (strcmp(node->name, "service_manager.elf") == 0 ||
            strcmp(node->name, "compositor.elf") == 0 ||
            strcmp(node->name, "panel.elf") == 0 ||
            strcmp(node->name, "keyboard_driver.elf") == 0 ||
            strcmp(node->name, "mouse_driver.elf") == 0 ||
            strcmp(node->name, "init.elf") == 0) {
            continue;
        }

        uint32_t parent_ino = EXT2_ROOT_INO;
        int len = strlen(node->name);
        
        // Skip drivers (already installed)
        if (len > 2 && strcmp(node->name + len - 2, ".o") == 0) {
             continue;
        }

        // Check if file already exists on disk
        if (ext2_dir_lookup(ext2_root_fs, parent_ino, node->name) != 0) {
            continue;
        }

        uint8_t *buf = kmalloc(node->length);
        if (buf) {
            vfs_read(node, 0, node->length, buf);
            
            // Write to disk (blocks on AHCI interrupt)
            uint32_t ino = ext2_create(ext2_root_fs, parent_ino, node->name, EXT2_S_IFREG | 0755);
            if (ino) {
                ext2_inode_t inode;
                ext2_read_inode(ext2_root_fs, ino, &inode);
                ext2_write_data(ext2_root_fs, ino, &inode, 0, node->length, buf);
            }
            kfree(buf);
        }
    }
    
    // Create marker file
    ext2_create(ext2_root_fs, EXT2_ROOT_INO, "kernel_installed", EXT2_S_IFREG | 0644);
    ext2_sync(ext2_root_fs);
    kprintf("[INSTALL] Background Installation Complete.\n");
    
    task_exit();
}

// Helper to scan and load drivers from /disk/drivers
static void load_drivers_from_disk(void) {
    if (!vfs_root) return;
    
    // Check if /drivers exists
    vfs_node_t *drivers_dir = vfs_finddir(vfs_root, "drivers");
    if (!drivers_dir) {
        // Try creating it if we are on disk
        if (vfs_root == ext2_get_root(ext2_root_fs)) {
            ext2_create(ext2_root_fs, EXT2_ROOT_INO, "drivers", EXT2_S_IFDIR | 0755);
            drivers_dir = vfs_finddir(vfs_root, "drivers");
        }
    }
    
    if (!drivers_dir) return;
    
    kprintf("[KERNEL] Scanning /drivers for modules...\n");
    
    for (int i = 0; ; i++) {
        vfs_node_t *node = vfs_readdir(drivers_dir, i);
        if (!node) break;
        
        if (strcmp(node->name, ".") == 0 || strcmp(node->name, "..") == 0) continue;
        
        // Check for .o or .ko extension
        int len = strlen(node->name);
        if (len > 2 && strcmp(node->name + len - 2, ".o") == 0) {
            kprintf("  - Loading module: %s... ", node->name);
            
            void *file_data = kmalloc(node->length);
            if (file_data) {
                if (vfs_read(node, 0, node->length, file_data) == node->length) {
                    void *entry_point = NULL;
                    int res = elf_load_module(file_data, node->length, &entry_point);
                    if (res == 0) {
                        if (entry_point) {
                            kprintf("OK (Entry at %p)\n", entry_point);
                            // Call entry point
                            void (*driver_init_func)(void) = (void (*)(void))entry_point;
                            driver_init_func();
                        } else {
                            kprintf("OK (No entry point)\n");
                        }
                    } else {
                        kprintf("FAILED (Error %d)\n", res);
                    }
                } else {
                    kprintf("FAILED (Read Error)\n");
                }
                // Note: we don't free file_data if loaded successfully because sections point to it?
                // Actually elf_load_module allocates new memory for sections, so we can free the file buffer.
                kfree(file_data); 
            } else {
                kprintf("FAILED (OOM)\n");
            }
        }
    }
}

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
    klog_init(); // Initialize kernel log buffer
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

    // Parse ACPI tables (needed before IOAPIC and RTC)
    acpi_init();

    // Initialize SMP (Detect and wake up other cores)
    smp_init();

    // Initialize IOAPIC (replaces PIC for IRQ routing)
    ioapic_init();
    if (acpi_get_info()->has_ioapic) {
        lapic_set_ioapic_mode(1);
    }

    // Initialize Timer (PIT for scheduling — used for LAPIC calibration too)
    timer_init();

    // Initialize Scheduler
    scheduler_init();

    // Enable Interrupts AFTER scheduler is ready to handle ticks
    __asm__ volatile ("sti");

    // Calibrate and start LAPIC timer (replaces PIT for scheduling)
    if (acpi_get_info()->has_ioapic) {
        lapic_timer_calibrate();
        lapic_timer_start();
    }

    // Initialize RTC (for date/time)
    rtc_init();

    // Initialize driver subsystem
    driver_init();

    // Initialize Kernel Symbol Table
    ksyms_init();
    ksyms_register("kprintf", (void*)kprintf);
    ksyms_register("kmalloc", (void*)kmalloc);
    ksyms_register("kfree", (void*)kfree);
    ksyms_register("vfs_finddir", (void*)vfs_finddir);
    ksyms_register("vfs_read", (void*)vfs_read);
    ksyms_register("driver_register", (void*)driver_register);
    ksyms_register("driver_find", (void*)driver_find);
    ksyms_register("driver_register_irq", (void*)driver_register_irq);
    ksyms_register("pci_find_device", (void*)pci_find_device);
    ksyms_register("pci_config_read8", (void*)pci_config_read8);
    ksyms_register("pci_config_read16", (void*)pci_config_read16);
    ksyms_register("pci_config_read32", (void*)pci_config_read32);
    ksyms_register("pci_config_write16", (void*)pci_config_write16);
    ksyms_register("pci_config_write32", (void*)pci_config_write32);
    ksyms_register("port_inb", (void*)port_inb);
    ksyms_register("port_outb", (void*)port_outb);
    ksyms_register("port_inl", (void*)port_inl);
    ksyms_register("port_outl", (void*)port_outl);
    ksyms_register("fb_ptr", (void*)&fb_ptr); 
    ksyms_register("fb_width", (void*)&fb_width);
    ksyms_register("fb_height", (void*)&fb_height);
    ksyms_register("memcpy", (void*)memcpy);
    ksyms_register("memset", (void*)memset);
    ksyms_register("strcmp", (void*)strcmp);
    ksyms_register("strlen", (void*)strlen);
    
    // Initialize PCI bus enumeration
    pci_init();

    // Initialize AHCI Storage
    ahci_init();

    // Initialize E1000 Network Driver
    e1000_init();
    
    // Initialize lwIP Network Interface (Adapter)
    // We pass NULL for now as we don't have the full lwIP stack initialized
    void ethernetif_init(void *netif); // Forward decl
    ethernetif_init(NULL);

    // Initialize BGA Graphics (if available)
    bga_init();

    // Initialize Audio (HDA)
    hda_init();

    // Initialize block device layer and register AHCI
    block_init();

    // Probe for partitions on sda
    block_device_t *sda = block_find("sda");
    if (sda) {
        partition_probe(sda);
    }

    // Initialize buffer cache
    bcache_init();

    // Try to mount ext2 filesystem on first partition
    block_device_t *sda1 = block_find("sda1");
    if (sda1) {
        ext2_root_fs = ext2_mount(sda1);
        if (ext2_root_fs) {
            kprintf("[KERNEL] ext2 filesystem mounted on sda1\n");
        }
    }
    if (!ext2_root_fs) {
        // Try raw disk (no partition table)
        block_device_t *sda_raw = block_find("sda");
        if (sda_raw) {
            ext2_root_fs = ext2_mount(sda_raw);
            if (ext2_root_fs) {
                kprintf("[KERNEL] ext2 filesystem mounted on sda (raw)\n");
            }
        }
    }

    // Initialize /dev filesystem
    devfs_init();

    // Initialize IPC subsystem
    ipc_init();

    // Initialize security subsystem (capability-based)
    security_init();

    // Initialize syscalls (MSRs)
    syscall_init();

    // Initialize AIO
    aio_init();

    kprintf("[KERNEL] fb_ptr=%p, width=%lu, height=%lu\n", fb_ptr, fb_width, fb_height);

    // Draw the "Sony Blue" background
    if (fb_ptr) {
        kprintf("[KERNEL] Clearing screen...\n");
        for (size_t i = 0; i < fb_width * fb_height; i++) {
            fb_ptr[i] = 0xFF003366; 
        }
        kprintf("[KERNEL] Screen cleared.\n");
    }

    // Initialize Kernel Log Buffer
    klog_init();

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
        
        // Mount disk at /disk
        if (ext2_root_fs) {
            if (vfs_mount("/disk", ext2_get_root(ext2_root_fs)) == 0) {
                console_printf(" VFS: Mounted /dev/sda at /disk\n");
                
                // INSTALLATION: Copy initrd files to disk
                console_printf(" INSTALL: Checking installation...\n");
                
                // Check if already installed (look for kernel_installed marker)
                if (0) { // Disabled for now to fix boot hang
                    console_printf(" INSTALL: First boot detected. Installing system files to disk...\n");
                    
                    // Create essential directories
                    ext2_create(ext2_root_fs, EXT2_ROOT_INO, "dev", EXT2_S_IFDIR | 0755);
                    ext2_create(ext2_root_fs, EXT2_ROOT_INO, "tmp", EXT2_S_IFDIR | 0755);
                    ext2_create(ext2_root_fs, EXT2_ROOT_INO, "mnt", EXT2_S_IFDIR | 0755);
                    ext2_create(ext2_root_fs, EXT2_ROOT_INO, "drivers", EXT2_S_IFDIR | 0755);
                    
                    // 1. Install Critical Files Synchronously
                    const char *critical_files[] = {
                        "service_manager.elf",
                        "compositor.elf",
                        "panel.elf",
                        "keyboard_driver.elf",
                        "mouse_driver.elf",
                        "init.elf",
                        NULL
                    };

                    for (int i=0; critical_files[i]; i++) {
                        vfs_node_t *node = vfs_finddir(vfs_root, critical_files[i]);
                        if (node) {
                             console_printf("   - %s... ", critical_files[i]);
                             uint8_t *buf = kmalloc(node->length);
                             if (buf) {
                                 vfs_read(node, 0, node->length, buf);
                                 uint32_t ino = ext2_create(ext2_root_fs, EXT2_ROOT_INO, critical_files[i], EXT2_S_IFREG | 0755);
                                 if (ino) {
                                     ext2_inode_t inode;
                                     ext2_read_inode(ext2_root_fs, ino, &inode);
                                     ext2_write_data(ext2_root_fs, ino, &inode, 0, node->length, buf);
                                     console_printf("OK\n");
                                 } else {
                                     console_printf("FAIL\n");
                                 }
                                 kfree(buf);
                             }
                        }
                    }

                    // 2. Install Drivers Synchronously (so we can load them)
                    for (size_t i = 0; ; i++) {
                        vfs_node_t *node = vfs_readdir(vfs_root, i);
                        if (!node) break;
                        
                        int len = strlen(node->name);
                        if (len > 2 && strcmp(node->name + len - 2, ".o") == 0) {
                            console_printf("   - Driver %s... ", node->name);
                            uint32_t drv_dir = ext2_dir_lookup(ext2_root_fs, EXT2_ROOT_INO, "drivers");
                            if (!drv_dir) drv_dir = EXT2_ROOT_INO;
                            
                             uint8_t *buf = kmalloc(node->length);
                             if (buf) {
                                 vfs_read(node, 0, node->length, buf);
                                 uint32_t ino = ext2_create(ext2_root_fs, drv_dir, node->name, EXT2_S_IFREG | 0755);
                                 if (ino) {
                                     ext2_inode_t inode;
                                     ext2_read_inode(ext2_root_fs, ino, &inode);
                                     ext2_write_data(ext2_root_fs, ino, &inode, 0, node->length, buf);
                                     console_printf("OK\n");
                                 }
                                 kfree(buf);
                             }
                        }
                    }

                    // 3. Spawn Background Task for the rest (apps, etc.)
                    initrd_root_node = vfs_root; 
                    task_create("installer", background_installer_task);
                    console_printf(" INSTALL: Background installer started for remaining files.\n");

                } else {
                    console_printf(" INSTALL: System already installed.\n");
                    
                    // Ensure /dev exists even if installed (repair)
                    if (ext2_dir_lookup(ext2_root_fs, EXT2_ROOT_INO, "dev") == 0) {
                        ext2_create(ext2_root_fs, EXT2_ROOT_INO, "dev", EXT2_S_IFDIR | 0755);
                    }
                }
                
                if (0) { // Disabled for now to stay on initrd
                // BOOT FROM DISK
                console_printf(" VFS: Switching root filesystem to /dev/sda1...\n");
                vfs_root = ext2_get_root(ext2_root_fs);
                }
                
                // Mount devfs
                if (vfs_mount("/dev", devfs_get_root()) == 0) {
                    console_printf(" VFS: Mounted devfs at /dev\n");
                } else {
                    console_printf(" VFS: Failed to mount devfs at /dev\n");
                }
                
                // Load Native Drivers
                load_drivers_from_disk();
                
            } else {
                 console_printf(" VFS: Failed to mount /disk\n");
            }
        }

        // List files in initrd (now invalid if we switched root, but let's skip or list new root)
        console_printf("\n Files in root (/%s):\n", vfs_root == ext2_get_root(ext2_root_fs) ? "sda1" : "initrd");
        for (size_t i = 0; ; i++) {
            vfs_node_t *node = vfs_readdir(vfs_root, i);
            if (node == NULL) break;
            console_printf("   - %s (%lu bytes)\n", node->name, node->length);
        }
        
        // Launch Service Manager
        vfs_node_t *sm_node = vfs_finddir(vfs_root, "service_manager.elf");
        if (sm_node) {
            console_printf("[KERNEL] Found service_manager.elf, loading...\n");
            void *sm_data = kmalloc(sm_node->length);
            if (sm_data) {
                vfs_read(sm_node, 0, sm_node->length, (uint8_t*)sm_data);
                
                int pid = task_create_user("service_manager", sm_data, sm_node->length, 0);
                if (pid >= 0) {
                    console_printf("[KERNEL] Service Manager started (PID %d)\n", pid);
                } else {
                    console_printf("[KERNEL] Failed to start Service Manager\n");
                }
                kfree(sm_data);
            }
        } else {
            console_printf("[KERNEL] service_manager.elf not found in initrd!\n");
            
            // Fallback to init.elf
            vfs_node_t *init_node = initrd_find("init.elf");
            if (init_node) {
                 console_printf("[KERNEL] Falling back to init.elf...\n");
                 void *init_data = kmalloc(init_node->length);
                 if (init_data) {
                     vfs_read(init_node, 0, init_node->length, (uint8_t*)init_data);
                     task_create_user("init", init_data, init_node->length, 0);
                     kfree(init_data);
                 }
            }
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
