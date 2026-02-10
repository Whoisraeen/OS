#include "bga.h"
#include "io.h"
#include "console.h"
#include "vmm.h"
#include "pmm.h"

static pci_device_t *bga_dev = NULL;
static uint32_t *lfb_virt = NULL;

static void bga_write_register(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t bga_read_register(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

void bga_set_resolution(uint16_t width, uint16_t height, uint16_t bpp) {
    bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write_register(VBE_DISPI_INDEX_XRES, width);
    bga_write_register(VBE_DISPI_INDEX_YRES, height);
    bga_write_register(VBE_DISPI_INDEX_BPP, bpp);
    bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    
    // Update global framebuffer info for the console
    extern uint32_t *fb_ptr;
    extern uint64_t fb_width;
    extern uint64_t fb_height;
    
    // Note: We need to remap the LFB if the resolution change affects size significantly,
    // but typically the BAR size is fixed (e.g. 16MB).
    // Assuming lfb_virt is still valid.
    fb_ptr = lfb_virt;
    fb_width = width;
    fb_height = height;
    
    // Clear screen
    console_clear();
    console_printf("[BGA] Resolution set to %dx%dx%d\n", width, height, bpp);
}

void bga_init(void) {
    // 1. Find the device (QEMU/Bochs Graphics)
    // Vendor: 0x1234, Device: 0x1111
    bga_dev = pci_find_device(0x1234, 0x1111);
    
    if (!bga_dev) {
        console_printf("[BGA] Device not found.\n");
        return;
    }
    
    console_printf("[BGA] Found BGA at %02x:%02x.%d\n", 
        bga_dev->bus, bga_dev->slot, bga_dev->func);
        
    // 2. Map the Linear Framebuffer (LFB) - BAR0
    uint32_t bar0_size = 0;
    uint64_t bar0_phys = pci_get_bar_address(bga_dev, 0, &bar0_size);
    
    if (bar0_phys == 0) {
        console_printf("[BGA] Invalid BAR0.\n");
        return;
    }
    
    // Map 16MB (typical BGA LFB size)
    // In a real OS, use bar0_size, but for now we hardcode reasonable limit
    uint64_t map_size = 16 * 1024 * 1024; 
    
    // We need to map this physical memory to virtual memory
    // Since it's MMIO, we can pick a high address or use HHDM if it covers it?
    // HHDM usually only covers RAM. We need to map MMIO specifically.
    // For simplicity in this kernel, let's map it page-by-page to a known region
    // OR just rely on identity mapping if we had it.
    // Let's assume we can map it dynamically.
    
    // Quick hack: Use a fixed virtual address for LFB mapping
    // 0xFFFFFFFFFC000000 is high up (-64MB)
    uint64_t virt_base = 0xFFFFFFFFFC000000;
    
    // Ensure we don't cross page boundaries incorrectly (not an issue for 4k loop)
    for (uint64_t offset = 0; offset < map_size; offset += 4096) {
        // Use PTE_NOCACHE for MMIO
        vmm_map_page(virt_base + offset, bar0_phys + offset, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);
    }
    
    lfb_virt = (uint32_t *)virt_base;
    console_printf("[BGA] LFB mapped at 0x%lx (Phys 0x%lx)\n", virt_base, bar0_phys);
    
    // 3. Check BGA Version
    uint16_t version = bga_read_register(VBE_DISPI_INDEX_ID);
    console_printf("[BGA] Version: 0x%x\n", version);
    
    // 4. Test: Switch resolution to 1024x768x32
    console_printf("[BGA] Switching resolution to 1024x768x32...\n");
    bga_set_resolution(1024, 768, 32);
}
