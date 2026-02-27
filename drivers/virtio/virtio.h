#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include <stddef.h>
#include "pci.h"

// Virtio Constants
#define VIRTIO_VENDOR_ID            0x1AF4

// Device IDs (Legacy / Modern transition IDs)
#define VIRTIO_DEVICE_NET           0x1000
#define VIRTIO_DEVICE_BLOCK         0x1001
#define VIRTIO_DEVICE_CONSOLE       0x1003
#define VIRTIO_DEVICE_ENTROPY       0x1005
#define VIRTIO_DEVICE_GPU           0x1050 

// Virtio Status Byte
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED        128

// Virtqueue Constants
#define VIRTQ_DESC_F_NEXT           1
#define VIRTQ_DESC_F_WRITE          2
#define VIRTQ_DESC_F_INDIRECT       4


// Standard layout of a virtqueue descriptor
typedef struct {
    uint64_t addr;   // Address (guest-physical)
    uint32_t len;    // Length
    uint16_t flags;  // The flags as indicated above
    uint16_t next;   // Next field if flags & NEXT
} __attribute__((packed)) virtq_desc_t;

// Available path
typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) virtq_avail_t;

// Virtio 1.0 PCI Capabilities
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define VIRTIO_PCI_CAP_PCI_CFG    5

struct virtio_pci_cap {
    uint8_t cap_vndr;   // Generic PCI field: PCI_CAP_ID_VNDR (0x09)
    uint8_t cap_next;   // Generic PCI field: next ptr
    uint8_t cap_len;    // Generic PCI field: capability length
    uint8_t cfg_type;   // Identifies the structure.
    uint8_t bar;        // Where to find it.
    uint8_t padding[3]; // Pad to full dword
    uint32_t offset;    // Offset within bar.
    uint32_t length;    // Length of the structure, in bytes.
} __attribute__((packed));

struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;
    uint32_t notify_off_multiplier; // Multiplier for queue_notify_off.
} __attribute__((packed));

struct virtio_pci_common_cfg {
    // About the whole device.
    uint32_t device_feature_select; // read-write
    uint32_t device_feature;        // read-only
    uint32_t driver_feature_select; // read-write
    uint32_t driver_feature;        // read-write
    uint16_t msix_config;           // read-write
    uint16_t num_queues;            // read-only
    uint8_t device_status;          // read-write
    uint8_t config_generation;      // read-only
    
    // About a specific virtqueue.
    uint16_t queue_select;          // read-write
    uint16_t queue_size;            // read-write
    uint16_t queue_msix_vector;     // read-write
    uint16_t queue_enable;          // read-write
    uint16_t queue_notify_off;      // read-only
    uint32_t queue_desc_lo;         // read-write
    uint32_t queue_desc_hi;         // read-write
    uint32_t queue_avail_lo;        // read-write
    uint32_t queue_avail_hi;        // read-write
    uint32_t queue_used_lo;         // read-write
    uint32_t queue_used_hi;         // read-write
} __attribute__((packed));

// Used Ring Element
typedef struct {
    uint32_t id;     // Index of start of used descriptor chain
    uint32_t len;    // Total length of the descriptor chain which was used (written to)
} __attribute__((packed)) virtq_used_elem_t;

// Used Ring
typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

// Context for a single queue
typedef struct {
    uint16_t queue_index;
    uint16_t num_desc;       // Number of descriptors (ring size)
    uint16_t last_used_idx;  // Last processed used index by driver
    
    // Memory for the queue (virtually addressed in kernel)
    void *desc_virt;
    void *avail_virt;
    void *used_virt;
    
    // Physical addresses given to device
    uint64_t desc_phys;
    uint64_t avail_phys;
    uint64_t used_phys;
    
    // Helper to track free descriptors (simple list or bitmap)
    uint16_t free_head;      // Index of first free descriptor
    uint16_t num_free;       // Count of free descriptors
    
    // We also need to shadow `next` for free list management if we modify descriptors
    // but usually we just read `desc[i].next`.
    
    // Notify offset (Modern)
    uint64_t notify_offset; 
} virtio_queue_t;

// Functions
void virtio_init(void);
int virtio_find_device(uint16_t device_id, pci_device_t **out_pci);
void virtio_reset_device(pci_device_t *pci);
void virtio_set_status(pci_device_t *pci, uint8_t status);
uint8_t virtio_get_status(pci_device_t *pci);
uint64_t virtio_get_features(pci_device_t *pci);
void virtio_set_features(pci_device_t *pci, uint64_t features);
int virtio_queue_setup(pci_device_t *pci, virtio_queue_t *vq, uint16_t index);
void virtio_notify_queue(virtio_queue_t *vq);
size_t virtio_queue_size_bytes(uint16_t qsize);

#endif
