#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>
#include <stddef.h>

// Maximum registered drivers
#define MAX_DRIVERS 32

// Maximum IRQ vectors we can dispatch
#define MAX_IRQ_HANDLERS 48

// Driver types
typedef enum {
    DRIVER_CHAR,       // Character device (keyboard, serial, mouse)
    DRIVER_BLOCK,      // Block device (disk)
    DRIVER_NET,        // Network device
    DRIVER_DISPLAY,    // Display/GPU
    DRIVER_OTHER
} driver_type_t;

// Forward declaration
struct driver;

// Driver operations
typedef struct driver_ops {
    int (*init)(struct driver *drv);
    int (*read)(struct driver *drv, uint8_t *buf, size_t count, size_t offset);
    int (*write)(struct driver *drv, const uint8_t *buf, size_t count, size_t offset);
    int (*ioctl)(struct driver *drv, uint32_t cmd, void *arg);
    void (*irq_handler)(struct driver *drv);
    void (*shutdown)(struct driver *drv);
} driver_ops_t;

// Driver structure
typedef struct driver {
    char name[32];              // Driver name (e.g. "keyboard", "e1000")
    driver_type_t type;         // Device type
    driver_ops_t *ops;          // Operations
    void *private_data;         // Driver-specific data
    uint8_t irq_vector;         // Primary IRQ vector (0 = none)
    int registered;             // 1 if active
    uint32_t pci_vendor;        // PCI vendor ID (0 if not PCI)
    uint32_t pci_device;        // PCI device ID
} driver_t;

// Initialize driver subsystem
void driver_init(void);
void driver_init_pci(void);

// Register a driver (returns driver index or -1)
int driver_register(driver_t *drv);

// Unregister a driver
void driver_unregister(int index);

// Find driver by name
driver_t *driver_find(const char *name);

// Find driver by PCI vendor/device
driver_t *driver_find_pci(uint32_t vendor, uint32_t device);

// Register an IRQ handler for a specific vector
void driver_register_irq(uint8_t vector, driver_t *drv);

// Dispatch IRQ to registered driver (called from idt.c)
// Returns 1 if handled, 0 if no handler
int driver_dispatch_irq(uint8_t vector);

// List all registered drivers (debug)
void driver_list(void);

#endif
