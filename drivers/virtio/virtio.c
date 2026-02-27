#include "virtio.h"
#include "pci.h"
#include "io.h"  
#include "klog.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "serial.h" // For kprintf

// Better helper that walks properly
static void *get_virtio_cfg(pci_device_t *pci, uint8_t type) {
    uint8_t cap_ptr = pci_config_read8(pci->bus, pci->slot, pci->func, 0x34);
    while (cap_ptr != 0) {
        uint8_t cap_id = pci_config_read8(pci->bus, pci->slot, pci->func, cap_ptr);
        if (cap_id == 0x09) { // VENDOR SPECIFIC (Virtio)
             uint8_t cfg_type = pci_config_read8(pci->bus, pci->slot, pci->func, cap_ptr + 3);
             if (cfg_type == type) {
                 uint8_t bar_idx = pci_config_read8(pci->bus, pci->slot, pci->func, cap_ptr + 4);
                 uint32_t offset = pci_config_read32(pci->bus, pci->slot, pci->func, cap_ptr + 8);
                 uint64_t bar_phys = pci_get_bar_address(pci, bar_idx, NULL);
                 return (void*)(bar_phys + offset + vmm_get_hhdm_offset());
             }
        }
        cap_ptr = pci_config_read8(pci->bus, pci->slot, pci->func, cap_ptr + 1);
    }
    return NULL;
}


int virtio_find_device(uint16_t device_id, pci_device_t **out_pci) {
    pci_device_t *dev = pci_find_device(VIRTIO_VENDOR_ID, device_id);
    if (dev) {
        *out_pci = dev;
        // Enable Bus Master & Memory
        pci_enable_bus_master(dev);
        pci_enable_memory(dev);
        return 1;
    }
    return 0;
}

void virtio_reset_device(pci_device_t *pci) {
    volatile struct virtio_pci_common_cfg *cfg = get_virtio_cfg(pci, VIRTIO_PCI_CAP_COMMON_CFG);
    if (!cfg) return;
    cfg->device_status = 0;
    while(cfg->device_status != 0); // Wait for reset
}

void virtio_set_status(pci_device_t *pci, uint8_t status) {
    volatile struct virtio_pci_common_cfg *cfg = get_virtio_cfg(pci, VIRTIO_PCI_CAP_COMMON_CFG);
    if (!cfg) return;
    cfg->device_status |= status; 
}

uint8_t virtio_get_status(pci_device_t *pci) {
    volatile struct virtio_pci_common_cfg *cfg = get_virtio_cfg(pci, VIRTIO_PCI_CAP_COMMON_CFG);
    if (!cfg) return 0;
    return cfg->device_status;
}

uint64_t virtio_get_features(pci_device_t *pci) {
    volatile struct virtio_pci_common_cfg *cfg = get_virtio_cfg(pci, VIRTIO_PCI_CAP_COMMON_CFG);
    if (!cfg) return 0;
    
    cfg->device_feature_select = 0;
    uint64_t f = cfg->device_feature;
    cfg->device_feature_select = 1;
    f |= ((uint64_t)cfg->device_feature << 32);
    return f;
}

void virtio_set_features(pci_device_t *pci, uint64_t features) {
    volatile struct virtio_pci_common_cfg *cfg = get_virtio_cfg(pci, VIRTIO_PCI_CAP_COMMON_CFG);
    if (!cfg) return;
    
    cfg->driver_feature_select = 0;
    cfg->driver_feature = (uint32_t)features;
    cfg->driver_feature_select = 1;
    cfg->driver_feature = (uint32_t)(features >> 32);
}

size_t virtio_queue_size_bytes(uint16_t qsize) {
    return 16 * qsize + 6 + 2 * qsize + 6 + 8 * qsize; 
}

int virtio_queue_setup(pci_device_t *pci, virtio_queue_t *vq, uint16_t index) {
    volatile struct virtio_pci_common_cfg *cfg = get_virtio_cfg(pci, VIRTIO_PCI_CAP_COMMON_CFG);
    if (!cfg) {
        kprintf("[VIRTIO] Failed to get Common Config for Queue %d\n", index);
        return 0;
    }
    
    cfg->queue_select = index;
    uint16_t size = cfg->queue_size;
    
    if (size == 0 || size > 32768) {
        kprintf("[VIRTIO] Queue %d not available (size=%d)\n", index, size);
        return 0;
    }
    
    // Calculate sizes
    size_t sz_desc = 16 * size;
    size_t sz_avail = 6 + 2 * size;
    size_t sz_used = 6 + 8 * size;
    
    size_t total_size = sz_desc + sz_avail;
    size_t used_offset = (total_size + 4095) & ~4095;
    total_size = used_offset + sz_used;
    
    size_t pages = (total_size + 4095) / 4096;
    void *phys_addr = pmm_alloc_pages(pages);
    if (!phys_addr) return 0;
    
    // Map to virtual memory
    void *virt_addr = (void*)((uint64_t)phys_addr + vmm_get_hhdm_offset()); 
    memset(virt_addr, 0, total_size);
    
    vq->queue_index = index;
    vq->num_desc = size;
    vq->desc_virt = virt_addr;
    vq->avail_virt = (void*)((uint8_t*)virt_addr + sz_desc);
    vq->used_virt = (void*)((uint8_t*)virt_addr + used_offset);
    
    vq->desc_phys = (uint64_t)phys_addr;
    vq->avail_phys = (uint64_t)phys_addr + sz_desc;
    vq->used_phys = (uint64_t)phys_addr + used_offset;
    
    virtq_desc_t *desc = (virtq_desc_t*)vq->desc_virt;
    for (int i = 0; i < size - 1; i++) {
        desc[i].next = i + 1;
        desc[i].flags = 0; 
    }
    desc[size-1].next = 0; 
    
    vq->num_free = size;
    vq->free_head = 0;
    vq->last_used_idx = 0;
    
    // Give to device (Modern Interface)
    cfg->queue_desc_lo = (uint32_t)vq->desc_phys;
    cfg->queue_desc_hi = (uint32_t)(vq->desc_phys >> 32);
    cfg->queue_avail_lo = (uint32_t)vq->avail_phys;
    cfg->queue_avail_hi = (uint32_t)(vq->avail_phys >> 32);
    cfg->queue_used_lo = (uint32_t)vq->used_phys;
    cfg->queue_used_hi = (uint32_t)(vq->used_phys >> 32);
    
    cfg->queue_enable = 1;
    
    // Store notify offset
    // Find notify cap
    uint8_t cap_ptr = pci_config_read8(pci->bus, pci->slot, pci->func, 0x34);
    while (cap_ptr != 0) {
        uint8_t cap_id = pci_config_read8(pci->bus, pci->slot, pci->func, cap_ptr);
        if (cap_id == 0x09) { 
             uint8_t type = pci_config_read8(pci->bus, pci->slot, pci->func, cap_ptr + 3);
             if (type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                 // Found it
                 uint8_t bar_idx = pci_config_read8(pci->bus, pci->slot, pci->func, cap_ptr + 4);
                 uint32_t offset = pci_config_read32(pci->bus, pci->slot, pci->func, cap_ptr + 8);
                 uint32_t mult = pci_config_read32(pci->bus, pci->slot, pci->func, cap_ptr + 16);
                 
                 uint64_t bar_phys = pci_get_bar_address(pci, bar_idx, NULL);
                 uint64_t notify_base = bar_phys + offset + vmm_get_hhdm_offset();
                 
                 vq->notify_offset = notify_base + cfg->queue_notify_off * mult;
                 break;
             }
        }
        cap_ptr = pci_config_read8(pci->bus, pci->slot, pci->func, cap_ptr + 1);
    }
    
    kprintf("[VIRTIO] Queue %d setup. Size=%d, Notify=%llx\n", index, size, vq->notify_offset);
    return 1;
}

// New signature
void virtio_notify_queue(virtio_queue_t *vq) {
    if (vq->notify_offset) {
        // According to spec, we write index to the notify address (16-bit)
        *(volatile uint16_t *)vq->notify_offset = vq->queue_index;
    }
}
