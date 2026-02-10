#include "hda.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "serial.h"
#include "heap.h"
#include "string.h"
#include "timer.h"

// Intel HDA Controller State
typedef struct {
    pci_device_t *pci_dev;
    uint8_t *mmio_base;
    uint32_t *corb;
    uint64_t *rirb;
    uint32_t corb_entries;
    uint32_t rirb_entries;
    uint16_t corb_rp;
    uint16_t rirb_wp;
    
    // Codec Info
    int codec_addr;
} hda_state_t;

static hda_state_t hda;

static uint32_t hda_read32(uint32_t reg) {
    return *(volatile uint32_t *)(hda.mmio_base + reg);
}

static void hda_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(hda.mmio_base + reg) = val;
}

static uint16_t hda_read16(uint32_t reg) {
    return *(volatile uint16_t *)(hda.mmio_base + reg);
}

static void hda_write16(uint32_t reg, uint16_t val) {
    *(volatile uint16_t *)(hda.mmio_base + reg) = val;
}

static uint8_t hda_read8(uint32_t reg) {
    return *(volatile uint8_t *)(hda.mmio_base + reg);
}

static void hda_write8(uint32_t reg, uint8_t val) {
    *(volatile uint8_t *)(hda.mmio_base + reg) = val;
}

static void hda_reset_controller(void) {
    uint32_t gctl = hda_read32(HDA_GCTL);
    hda_write32(HDA_GCTL, gctl & ~1); // Clear CRST
    
    // Wait for reset
    int timeout = 1000;
    while ((hda_read32(HDA_GCTL) & 1) && --timeout) timer_sleep(1);
    
    hda_write32(HDA_GCTL, gctl | 1); // Set CRST
    
    timeout = 1000;
    while (!(hda_read32(HDA_GCTL) & 1) && --timeout) timer_sleep(1);
    
    // Wait for codecs to wake up
    timer_sleep(10);
}

// CORB/RIRB Setup would go here... simplified for now
// We will use Immediate Command Interface (ICW/ICR) for simplicity in this initial driver

static uint32_t hda_send_verb(int codec, int node, uint32_t verb, uint32_t payload) {
    uint32_t cmd = (codec << 28) | (node << 20) | (verb << 8) | payload;
    
    // Wait for ready
    while (hda_read16(HDA_ICIS) & 1);
    
    hda_write32(HDA_ICOI, cmd);
    hda_write16(HDA_ICIS, 1 | 2); // Set BUSY and IRV
    
    // Wait for done
    while (hda_read16(HDA_ICIS) & 1);
    
    if (hda_read16(HDA_ICIS) & 2) { // Valid result
        return hda_read32(HDA_ICII);
    }
    
    return 0xFFFFFFFF;
}

void hda_init(void) {
    // Find Intel HDA (Class 04, Subclass 03)
    pci_device_t *dev = pci_find_device_by_class(0x04, 0x03);
    if (!dev) {
        // Try to find by Vendor ID if class lookup fails or is generic
        dev = pci_find_device(0x8086, 0x2668); // ICH6
        if (!dev) return;
    }
    
    hda.pci_dev = dev;
    pci_enable_bus_master(dev);
    
    // Map MMIO (BAR0)
    uint32_t bar0_size;
    uint64_t bar0_phys = pci_get_bar_address(dev, 0, &bar0_size);
    hda.mmio_base = (uint8_t *)(bar0_phys + vmm_get_hhdm_offset());
    
    kprintf("[HDA] Found controller at %02x:%02x.%d, MMIO at 0x%lx\n",
        dev->bus, dev->slot, dev->func, bar0_phys);
        
    hda_reset_controller();
    
    uint16_t statests = hda_read16(HDA_STATESTS);
    kprintf("[HDA] Codec Status: %04x\n", statests);
    
    // Detect Codec
    for (int i = 0; i < 15; i++) {
        if (statests & (1 << i)) {
            hda.codec_addr = i;
            kprintf("[HDA] Codec found at address %d\n", i);
            
            uint32_t vid = hda_send_verb(i, 0, HDA_VERB_GET_PARAM, HDA_PARAM_VENDOR_ID);
            kprintf("[HDA] Codec Vendor ID: %08x\n", vid);
            break;
        }
    }
    
    // Initialize Streams (TODO)
}
