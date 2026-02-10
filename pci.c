#include "pci.h"
#include "io.h"
#include "serial.h"

// Discovered PCI devices
static pci_device_t devices[PCI_MAX_DEVICES];
static int device_count = 0;

// Forward declaration
static const char *pci_class_name(uint8_t class_code, uint8_t subclass);

// Build a PCI config address
static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1U << 31)                  // Enable bit
         | ((uint32_t)bus << 16)
         | ((uint32_t)(slot & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | ((uint32_t)offset & 0xFC);  // Aligned to 4 bytes
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset & 0xFC);
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset & 0xFC);
    return (val >> ((offset & 3) * 8)) & 0xFF;
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t addr = pci_address(bus, slot, func, offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    uint32_t old = inl(PCI_CONFIG_DATA);

    int shift = (offset & 2) * 8;
    old &= ~(0xFFFF << shift);
    old |= ((uint32_t)value << shift);

    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, old);
}

// Scan one function of a device
static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci_config_read16(bus, slot, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return; // No device

    if (device_count >= PCI_MAX_DEVICES) return;

    pci_device_t *dev = &devices[device_count];
    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = vendor;
    dev->device_id = pci_config_read16(bus, slot, func, PCI_DEVICE_ID);
    dev->class_code = pci_config_read8(bus, slot, func, PCI_CLASS);
    dev->subclass = pci_config_read8(bus, slot, func, PCI_SUBCLASS);
    dev->prog_if = pci_config_read8(bus, slot, func, PCI_PROG_IF);
    dev->revision = pci_config_read8(bus, slot, func, PCI_REVISION_ID);
    dev->header_type = pci_config_read8(bus, slot, func, PCI_HEADER_TYPE);
    dev->irq_line = pci_config_read8(bus, slot, func, PCI_INTERRUPT_LINE);
    dev->irq_pin = pci_config_read8(bus, slot, func, PCI_INTERRUPT_PIN);

    // Read BARs (only for header type 0)
    if ((dev->header_type & 0x7F) == 0) {
        for (int i = 0; i < 6; i++) {
            dev->bar[i] = pci_config_read32(bus, slot, func, PCI_BAR0 + i * 4);
        }
    }

    device_count++;

    kprintf("[PCI] %02x:%02x.%d  %04x:%04x  class=%02x:%02x  irq=%d  %s\n",
            bus, slot, func, dev->vendor_id, dev->device_id,
            dev->class_code, dev->subclass, dev->irq_line,
            pci_class_name(dev->class_code, dev->subclass));
}

// Get human-readable class name
static const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE Controller";
        case 0x06: return "SATA Controller";
        case 0x08: return "NVMe Controller";
        default:   return "Storage Controller";
        }
    case 0x02:
        switch (subclass) {
        case 0x00: return "Ethernet Controller";
        default:   return "Network Controller";
        }
    case 0x03:
        switch (subclass) {
        case 0x00: return "VGA Controller";
        default:   return "Display Controller";
        }
    case 0x04: return "Multimedia Device";
    case 0x05: return "Memory Controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x04: return "PCI-PCI Bridge";
        default:   return "Bridge Device";
        }
    case 0x07: return "Communication Controller";
    case 0x08: return "System Peripheral";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB Controller";
        default:   return "Serial Bus Controller";
        }
    case 0x0D: return "Wireless Controller";
    default:   return "Unknown";
    }
}

// Scan all buses
static void pci_scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint16_t vendor = pci_config_read16(bus, slot, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFF) continue;

        pci_scan_function(bus, slot, 0);

        // Check for multi-function device
        uint8_t header = pci_config_read8(bus, slot, 0, PCI_HEADER_TYPE);
        if (header & 0x80) {
            for (uint8_t func = 1; func < 8; func++) {
                pci_scan_function(bus, slot, func);
            }
        }
    }
}

void pci_init(void) {
    device_count = 0;
    kprintf("[PCI] Scanning buses...\n");

    // Check if host bridge is multi-function
    uint8_t header = pci_config_read8(0, 0, 0, PCI_HEADER_TYPE);
    if (header & 0x80) {
        // Multiple PCI host controllers
        for (uint8_t func = 0; func < 8; func++) {
            if (pci_config_read16(0, 0, func, PCI_VENDOR_ID) != 0xFFFF) {
                pci_scan_bus(func);
            }
        }
    } else {
        // Single bus domain — scan bus 0 and any bridges found
        pci_scan_bus(0);

        // Also scan secondary buses from PCI-PCI bridges
        for (int i = 0; i < device_count; i++) {
            if (devices[i].class_code == 0x06 && devices[i].subclass == 0x04) {
                uint8_t secondary = pci_config_read8(
                    devices[i].bus, devices[i].slot, devices[i].func, 0x19);
                if (secondary > 0) {
                    pci_scan_bus(secondary);
                }
            }
        }
    }

    kprintf("[PCI] Found %d devices\n", device_count);
}

void pci_enable_bus_master(pci_device_t *dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

void pci_enable_memory(pci_device_t *dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY_SPACE | PCI_CMD_IO_SPACE;
    pci_config_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

uint64_t pci_get_bar_address(pci_device_t *dev, int bar_index, uint32_t *size) {
    if (bar_index < 0 || bar_index > 5) return 0;

    uint32_t bar = dev->bar[bar_index];

    if (bar & PCI_BAR_IO) {
        // I/O BAR
        uint32_t addr = bar & ~0x3;
        if (size) {
            // Write all 1s to get size
            pci_config_write32(dev->bus, dev->slot, dev->func, PCI_BAR0 + bar_index * 4, 0xFFFFFFFF);
            uint32_t len = pci_config_read32(dev->bus, dev->slot, dev->func, PCI_BAR0 + bar_index * 4);
            pci_config_write32(dev->bus, dev->slot, dev->func, PCI_BAR0 + bar_index * 4, bar);
            *size = (~(len & ~0x3)) + 1;
        }
        return addr;
    } else {
        // Memory BAR
        int type = (bar >> 1) & 0x3;
        uint64_t addr = bar & ~0xF;
        
        if (type == 2) { // 64-bit
            uint32_t upper = dev->bar[bar_index + 1];
            addr |= ((uint64_t)upper << 32);
        }
        
        if (size) {
            pci_config_write32(dev->bus, dev->slot, dev->func, PCI_BAR0 + bar_index * 4, 0xFFFFFFFF);
            uint32_t len = pci_config_read32(dev->bus, dev->slot, dev->func, PCI_BAR0 + bar_index * 4);
            pci_config_write32(dev->bus, dev->slot, dev->func, PCI_BAR0 + bar_index * 4, bar);
            *size = (~(len & ~0xF)) + 1;
        }
        
        return addr;
    }
}

pci_device_t *pci_find_device_by_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].class_code == class_code && devices[i].subclass == subclass) {
            return &devices[i];
        }
    }
    return NULL; // Not found
}

pci_device_t *pci_find_device(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor && devices[i].device_id == device) {
            return &devices[i];
        }
    }
    return NULL;
}

int pci_get_device_count(void) {
    return device_count;
}

pci_device_t *pci_get_device(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

int pci_enable_msi(pci_device_t *dev, uint8_t vector, uint8_t processor) {
    uint16_t status = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_STATUS);
    if (!(status & (1 << 4))) return -1; // No capabilities list

    uint8_t cap_ptr = pci_config_read8(dev->bus, dev->slot, dev->func, 0x34);
    
    // Walk capabilities list
    while (cap_ptr != 0) {
        uint8_t cap_id = pci_config_read8(dev->bus, dev->slot, dev->func, cap_ptr);
        
        if (cap_id == PCI_CAP_ID_MSI) {
            // Found MSI capability
            uint16_t msg_ctrl = pci_config_read16(dev->bus, dev->slot, dev->func, cap_ptr + 2);
            
            // 64-bit capable?
            int is_64bit = (msg_ctrl & (1 << 7));
            
            // Configure Message Address
            // Destination ID (processor) goes in bits 12-19
            // RH=0, DM=0 (Physical Mode, Fixed Delivery)
            uint32_t addr = 0xFEE00000 | ((uint32_t)processor << 12);
            pci_config_write32(dev->bus, dev->slot, dev->func, cap_ptr + 4, addr);
            
            if (is_64bit) {
                pci_config_write32(dev->bus, dev->slot, dev->func, cap_ptr + 8, 0); // High 32 bits = 0
                pci_config_write16(dev->bus, dev->slot, dev->func, cap_ptr + 12, vector);
            } else {
                pci_config_write16(dev->bus, dev->slot, dev->func, cap_ptr + 8, vector);
            }
            
            // Enable MSI in Message Control
            // Bit 0 = MSI Enable
            // Bits 4-6 = Multiple Message Enable (We set to 0 to request 1 vector)
            msg_ctrl |= 1; 
            pci_config_write16(dev->bus, dev->slot, dev->func, cap_ptr + 2, msg_ctrl);
            
            return 0; // Success
        }
        
        // Next capability
        cap_ptr = pci_config_read8(dev->bus, dev->slot, dev->func, cap_ptr + 1);
    }
    
    return -1; // MSI not found
}
