#include "driver.h"
#include "serial.h"
#include "string.h"

// Registered drivers
static driver_t *drivers[MAX_DRIVERS];
static int driver_count = 0;

// IRQ dispatch table: vector -> driver
static driver_t *irq_handlers[MAX_IRQ_HANDLERS];

void driver_init(void) {
    for (int i = 0; i < MAX_DRIVERS; i++)
        drivers[i] = NULL;
    for (int i = 0; i < MAX_IRQ_HANDLERS; i++)
        irq_handlers[i] = NULL;
    driver_count = 0;
    kprintf("[DRIVER] Driver subsystem initialized\n");
}

// External driver init functions
extern void xhci_init(void *dev);
extern void nvme_init(void *dev);

// Simple hardcoded driver table for now
// In a real OS, drivers would register themselves
typedef struct {
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t prog_if_mask; // 0xFF = exact match, 0x00 = ignore
    void (*init)(void *dev);
    const char *name;
} pci_driver_def_t;

static pci_driver_def_t pci_drivers[] = {
    {0x0C, 0x03, 0x30, 0xFF, xhci_init, "XHCI USB"},
    {0x01, 0x08, 0x02, 0xFF, nvme_init, "NVMe Storage"},
};

#include "pci.h"

void driver_init_pci(void) {
    int count = pci_get_device_count();
    kprintf("[DRIVER] Probing %d PCI devices...\n", count);
    
    for (int i = 0; i < count; i++) {
        pci_device_t *dev = pci_get_device(i);
        if (!dev) continue;
        
        for (long unsigned int d = 0; d < sizeof(pci_drivers) / sizeof(pci_driver_def_t); d++) {
            pci_driver_def_t *drv = &pci_drivers[d];
            
            if (dev->class_code == drv->class_code &&
                dev->subclass == drv->subclass) {
                
                if ((dev->prog_if & drv->prog_if_mask) == (drv->prog_if & drv->prog_if_mask)) {
                    kprintf("[DRIVER] Matched '%s' for %02x:%02x.%d\n", drv->name, dev->bus, dev->slot, dev->func);
                    drv->init(dev);
                }
            }
        }
    }
}

int driver_register(driver_t *drv) {
    if (!drv || driver_count >= MAX_DRIVERS) return -1;

    // Find an empty slot
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (drivers[i] == NULL) {
            drivers[i] = drv;
            drv->registered = 1;
            driver_count++;

            // Register IRQ if specified
            if (drv->irq_vector > 0 && drv->irq_vector < MAX_IRQ_HANDLERS) {
                irq_handlers[drv->irq_vector] = drv;
            }

            kprintf("[DRIVER] Registered '%s' (type=%d, irq=%d)\n",
                    drv->name, drv->type, drv->irq_vector);
            return i;
        }
    }
    return -1;
}

void driver_unregister(int index) {
    if (index < 0 || index >= MAX_DRIVERS || !drivers[index]) return;

    driver_t *drv = drivers[index];

    // Remove IRQ handler
    if (drv->irq_vector > 0 && drv->irq_vector < MAX_IRQ_HANDLERS) {
        if (irq_handlers[drv->irq_vector] == drv)
            irq_handlers[drv->irq_vector] = NULL;
    }

    // Call shutdown if available
    if (drv->ops && drv->ops->shutdown)
        drv->ops->shutdown(drv);

    drv->registered = 0;
    drivers[index] = NULL;
    driver_count--;

    kprintf("[DRIVER] Unregistered '%s'\n", drv->name);
}

driver_t *driver_find(const char *name) {
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (drivers[i] && drivers[i]->registered) {
            // strcmp
            const char *a = drivers[i]->name;
            const char *b = name;
            int match = 1;
            while (*a && *b) {
                if (*a++ != *b++) { match = 0; break; }
            }
            if (match && *a == *b) return drivers[i];
        }
    }
    return NULL;
}

driver_t *driver_find_pci(uint32_t vendor, uint32_t device) {
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (drivers[i] && drivers[i]->registered &&
            drivers[i]->pci_vendor == vendor && drivers[i]->pci_device == device) {
            return drivers[i];
        }
    }
    return NULL;
}

void driver_register_irq(uint8_t vector, driver_t *drv) {
    if (vector < MAX_IRQ_HANDLERS) {
        irq_handlers[vector] = drv;
    }
}

int driver_dispatch_irq(uint8_t vector) {
    if (vector >= MAX_IRQ_HANDLERS) return 0;

    driver_t *drv = irq_handlers[vector];
    if (drv && drv->ops && drv->ops->irq_handler) {
        drv->ops->irq_handler(drv);
        return 1;
    }
    return 0;
}

void driver_list(void) {
    kprintf("[DRIVER] Registered drivers (%d):\n", driver_count);
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (drivers[i] && drivers[i]->registered) {
            driver_t *d = drivers[i];
            kprintf("  [%d] %s  type=%d  irq=%d",
                    i, d->name, d->type, d->irq_vector);
            if (d->pci_vendor)
                kprintf("  pci=%04x:%04x", d->pci_vendor, d->pci_device);
            kprintf("\n");
        }
    }
}
