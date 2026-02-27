#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// PCI config space I/O ports
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

// PCI config space registers (offsets)
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

// PCI Command register bits
#define PCI_CMD_IO_SPACE       (1 << 0)
#define PCI_CMD_MEMORY_SPACE   (1 << 1)
#define PCI_CMD_BUS_MASTER     (1 << 2)
#define PCI_CMD_INT_DISABLE    (1 << 10)

// BAR types
#define PCI_BAR_IO             0x01
#define PCI_BAR_MEM_32         0x00
#define PCI_BAR_MEM_64         0x04
#define PCI_BAR_PREFETCHABLE   0x08

// Maximum discovered devices
#define PCI_MAX_DEVICES 64

// PCI device info
typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    uint8_t irq_line;
    uint8_t irq_pin;
    uint32_t bar[6];
} pci_device_t;

// Initialize PCI (scan all buses)
void pci_init(void);

// Config space access
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);

// Enable bus mastering for a device
void pci_enable_bus_master(pci_device_t *dev);

// Enable memory space access
void pci_enable_memory(pci_device_t *dev);

// Get BAR address (handles 32-bit and 64-bit BARs)
// Returns physical address, sets *size if non-NULL
uint64_t pci_get_bar_address(pci_device_t *dev, int bar_index, uint32_t *size);

// Find device by class/subclass (returns NULL if not found)
pci_device_t *pci_find_device_by_class(uint8_t class_code, uint8_t subclass);

// Find device by vendor/device ID
pci_device_t *pci_find_device(uint16_t vendor, uint16_t device);

// Get number of discovered devices
int pci_get_device_count(void);

// PCI Capability IDs
#define PCI_CAP_ID_MSI       0x05
#define PCI_CAP_ID_MSIX      0x11

// Get device by index
pci_device_t *pci_get_device(int index);

// Enable MSI for a device
// vector: Interrupt vector to trigger (e.g., 46)
// processor: LAPIC ID of the processor to target (e.g., 0 for BSP)
// Returns 0 on success, -1 on failure
int pci_enable_msi(pci_device_t *dev, uint8_t vector, uint8_t processor);

// Find a PCI capability by ID
int pci_find_capability(pci_device_t *dev, uint8_t cap_id);

#endif
