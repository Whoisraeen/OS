#include "drivers/nvme.h"
#include "block.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "string.h"
#include "io.h"
#include "serial.h" // For kprintf

static nvme_t nvme;

// Forward Declarations
void nvme_read_sector(uint64_t lba, void *buf);
void nvme_write_sector(uint64_t lba, void *buf);

// Helper to convert physical to virtual (HHDM)
static inline void *p2v(uint64_t phys) {
    return (void *)(phys + vmm_get_hhdm_offset());
}

static inline uint64_t v2p(void *virt) {
    return (uint64_t)virt - vmm_get_hhdm_offset();
}

// MMIO Access
static inline uint32_t nvme_read_reg32(uint32_t reg) {
    return *(volatile uint32_t *)(nvme.base + reg);
}

static inline uint64_t nvme_read_reg64(uint32_t reg) {
    uint32_t lo = *(volatile uint32_t *)(nvme.base + reg);
    uint32_t hi = *(volatile uint32_t *)(nvme.base + reg + 4);
    return ((uint64_t)hi << 32) | lo;
}

static inline void nvme_write_reg32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(nvme.base + reg) = val;
}

static inline void nvme_write_reg64(uint32_t reg, uint64_t val) {
    *(volatile uint32_t *)(nvme.base + reg) = (uint32_t)val;
    *(volatile uint32_t *)(nvme.base + reg + 4) = (uint32_t)(val >> 32);
}

static void nvme_write_doorbell(uint16_t qid, uint16_t val) {
    // Doorbell Offset = 0x1000 + (2 * qid * (4 << CAP.DSTRD))
    uint32_t offset = NVME_DOORBELL_OFFSET + (qid * 2 * nvme.db_stride); 
    nvme_write_reg32(offset, val);
}

// Poll Completion Queue
// Returns status or 0xFFFF if timeout/error
static uint16_t nvme_poll_cq(uint16_t qid) {
    nvme_cq_entry_t *cq;
    uint16_t head;
    uint8_t phase;
    
    if (qid == 0) {
        cq = nvme.admin_cq;
        head = nvme.admin_cq_head;
        phase = nvme.admin_cq_phase;
    } else {
        cq = nvme.io_cq;
        head = nvme.io_cq_head;
        phase = nvme.io_cq_phase;
    }
    
    // Check Phase Tag (Bit 0 of status word 3)
    // Status is at offset 12 (bytes). 
    // Wait for Phase Tag match
    // Note: status is uint16_t at offset 14? No, struct has status at offset 14.
    // Let's rely on the struct definition.
    
    // Timeout loop needed? Using simple loop for now
    volatile nvme_cq_entry_t *entry = &cq[head];
    
    // Phase Tag (P) is bit 16 of Status (Status Field is upper 16 bits of DWord 3)
    // Actually, in standard NVM:
    // DWord 3:
    // Bits 31:17 = Status Code
    // Bit 16 = Phase Tag (P)
    // Status field in struct is uint16_t status (offset 14).
    // So P is bit 0 of `status` (if little endian? No, wait)
    
    // Standard says: Status Field is Bytes 14-15.
    // Byte 14: Phase Tag is Bit 0.
    // Wait, let's re-verify from spec or existing structs usually found online.
    // NVMe Spec 1.4, Figure 81: Completion Queue Entry - DWord 3
    // Bits 15:01 Status Field
    // Bit 00 Phase Tag (P)
    
    // So yes, bit 0 of the `status` field in our struct.
    
    int timeout = 1000000;
    while ((entry->status & 1) != phase) {
        timeout--;
        if (timeout == 0) return 0xFFFF;
        
        // Use standard memory barriers if we had them
        // asm volatile("pause");
    }
    
    // Process Entry
    uint16_t status = (entry->status >> 1); // Shift out Phase Tag
    
    // Update Head
    head++;
    if (head >= 64) { // Admin Queue Size
        head = 0;
        phase = !phase;
    }
    
    if (qid == 0) {
        nvme.admin_cq_head = head;
        nvme.admin_cq_phase = phase;
        
        // Ring CQ Doorbell
        // Offset = 1000h + ((2y + 1) * (4 << CAP.DSTRD))
        uint32_t offset = NVME_DOORBELL_OFFSET + ((qid * 2 + 1) * nvme.db_stride);
        nvme_write_reg32(offset, head);
    } else {
        nvme.io_cq_head = head;
        nvme.io_cq_phase = phase;
        
        uint32_t offset = NVME_DOORBELL_OFFSET + ((qid * 2 + 1) * nvme.db_stride);
        nvme_write_reg32(offset, head);
    }
    
    return status;
}

// Submit Command to Admin Queue
static int nvme_submit_admin_cmd(nvme_sq_entry_t *cmd) {
    // Copy to SQ
    uint16_t tail = nvme.admin_sq_tail;
    memcpy(&nvme.admin_sq[tail], cmd, sizeof(nvme_sq_entry_t));
    
    // Update Tail
    tail++;
    if (tail >= 64) tail = 0;
    nvme.admin_sq_tail = tail;
    
    // Ring Doorbell (SQ0 Tail)
    nvme_write_doorbell(0, tail);
    
    // Wait for completion (Polling)
    uint16_t status = nvme_poll_cq(0);
    return status;
}

// Block Ops
static int nvme_block_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf) {
    (void)dev;
    uint8_t *b = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        nvme_read_sector(lba + i, b + (i * 512));
    }
    return 0;
}

static int nvme_block_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    (void)dev;
    const uint8_t *b = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        nvme_write_sector(lba + i, (void *)(b + (i * 512)));
    }
    return 0;
}

static block_ops_t nvme_ops = {
    .read_sectors = nvme_block_read,
    .write_sectors = nvme_block_write
};

static block_device_t nvme_block_dev;

static void nvme_identify(void) {
    // Allocate DMA buffer for Identify Controller (4KB)
    uint64_t buf_phys = (uint64_t)pmm_alloc_page();
    void *buf_virt = p2v(buf_phys);
    memset(buf_virt, 0, 4096);
    
    nvme_sq_entry_t cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.prp1 = buf_phys;
    cmd.cdw10 = 1; // CNS = 1 (Identify Controller)
    
    uint16_t status = nvme_submit_admin_cmd(&cmd);
    
    if (status == 0) {
        nvme_identify_ctrl_t *ctrl = (nvme_identify_ctrl_t *)buf_virt;
        
        // Print Model & Serial
        // They are space padded, not null terminated usually.
        char model[41];
        memcpy(model, ctrl->mn, 40);
        model[40] = 0;
        
        char serial[21];
        memcpy(serial, ctrl->sn, 20);
        serial[20] = 0;
        
        // Trim spaces? Nah.
        kprintf("[NVMe] Identify Success. \n");
        kprintf("       Model:  '%s'\n", model);
        kprintf("       Serial: '%s'\n", serial);
        
        // Check NN (Number of Namespaces)
        // nvme.ns_count = ctrl->nn; 
        // Note: Identify Controller struct offset 516 (OACS) might be needed too.
        
    } else {
        kprintf("[NVMe] Identify Failed with Status 0x%x\n", status);
    }
    
    // Identify Namespace 1
    // Clear buffer
    memset(buf_virt, 0, 4096);
    cmd.cdw10 = 0; // CNS = 0 (Identify Namespace)
    cmd.nsid = 1;
    
    status = nvme_submit_admin_cmd(&cmd);
    if (status == 0) {
        // Namespace data at buf_virt
        // uint64_t nsze = *(uint64_t *)buf_virt; // Namespace Size (blocks)
        // uint64_t ncap = *(uint64_t *)(buf_virt + 8); // Namespace Capacity
        
        kprintf("[NVMe] Found Namespace 1.\n");
        
        // Register Block Device
        uint64_t nsze = *(uint64_t *)buf_virt; // Namespace Size (blocks)
        // uint64_t ncap = *(uint64_t *)(buf_virt + 8); // Namespace Capacity
        
        kprintf("[NVMe] Namespace 1 Size: %lu sectors (%lu MB)\n", nsze, (nsze * 512) / (1024*1024));
        
        // Setup Block Device
        nvme_block_dev.name[0] = 'n'; nvme_block_dev.name[1] = 'v'; nvme_block_dev.name[2] = 'm'; 
        nvme_block_dev.name[3] = 'e'; nvme_block_dev.name[4] = '0'; nvme_block_dev.name[5] = 'n';
        nvme_block_dev.name[6] = '1'; nvme_block_dev.name[7] = 0;
        
        nvme_block_dev.sector_size = 512; // Assuming LBA Fmt 0 is 512, which is typical for QEMU/Consumer
        nvme_block_dev.sector_count = nsze;
        nvme_block_dev.ops = &nvme_ops;
        nvme_block_dev.private_data = NULL;
        nvme_block_dev.parent = NULL;
        nvme_block_dev.partition_index = -1;
        
        int idx = block_register(&nvme_block_dev);
        if (idx >= 0) {
            kprintf("[NVMe] Registered 'nvme0n1'\n");
        }
    }
}



// Create IO CQ
static int nvme_create_io_cq(uint16_t qid, uint64_t phys_addr, uint16_t size) {
    nvme_sq_entry_t cmd = {0};
    cmd.opcode = NVME_ADMIN_CREATE_CQ;
    cmd.prp1 = phys_addr;
    cmd.cdw10 = ((size - 1) << 16) | qid;
    cmd.cdw11 = 1; // PC (Physically Contiguous) = 1, IEN = 0 (Polling)
    
    return nvme_submit_admin_cmd(&cmd);
}

// Create IO SQ
static int nvme_create_io_sq(uint16_t qid, uint16_t cqid, uint64_t phys_addr, uint16_t size) {
    nvme_sq_entry_t cmd = {0};
    cmd.opcode = NVME_ADMIN_CREATE_SQ;
    cmd.prp1 = phys_addr;
    cmd.cdw10 = ((size - 1) << 16) | qid;
    cmd.cdw11 = (cqid << 16) | 1; // PC = 1, Prio = 0
    
    return nvme_submit_admin_cmd(&cmd);
}


// Submit IO Command
static int nvme_submit_io_cmd(nvme_sq_entry_t *cmd) {
    // Copy to SQ1
    uint16_t tail = nvme.io_sq_tail;
    memcpy(&nvme.io_sq[tail], cmd, sizeof(nvme_sq_entry_t));
    
    // Update Tail
    tail++;
    if (tail >= 64) tail = 0;
    nvme.io_sq_tail = tail;
    
    // Ring Doorbell (SQ1 Tail)
    nvme_write_doorbell(1, tail);
    
    // Wait for completion (Polling CQ1)
    uint16_t status = nvme_poll_cq(1);
    return status;
}

static void nvme_setup_io_queues(void) {
    // Allocate Memory for IO Queues
    uint64_t sq_phys = (uint64_t)pmm_alloc_page();
    uint64_t cq_phys = (uint64_t)pmm_alloc_page();
    
    nvme.io_sq = (nvme_sq_entry_t *)p2v(sq_phys);
    nvme.io_cq = (nvme_cq_entry_t *)p2v(cq_phys);
    memset(nvme.io_sq, 0, 4096);
    memset(nvme.io_cq, 0, 4096);
    
    nvme.io_sq_phys = sq_phys;
    nvme.io_cq_phys = cq_phys;
    nvme.io_sq_tail = 0;
    nvme.io_cq_head = 0;
    nvme.io_cq_phase = 1;

    // Create CQ1
    if (nvme_create_io_cq(1, cq_phys, 64) != 0) {
        kprintf("[NVMe] Failed to create ACQ1\n");
        return;
    }
    
    // Create SQ1
    if (nvme_create_io_sq(1, 1, sq_phys, 64) != 0) {
        kprintf("[NVMe] Failed to create ABSQ1\n");
        return;
    }
    
    kprintf("[NVMe] IO Queues Created.\n");
}

void nvme_read_sector(uint64_t lba, void *buf) {
    // Create Read Command
    nvme_sq_entry_t cmd = {0};
    cmd.opcode = NVME_CMD_READ;
    cmd.nsid = 1;
    cmd.prp1 = v2p(buf); // Assume buffer is physically contiguous and < 4KB page boundary
                         // In real OS, handle PRP structure for large transfers.
    
    // SLBA (Starting LBA)
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    
    // NLB (Number of Logical Blocks) - 0's based (0 = 1 block)
    cmd.cdw12 = 0; 
    
    int status = nvme_submit_io_cmd(&cmd);
    if (status != 0) {
        kprintf("[NVMe] Read Error LBA %ld Status %x\n", lba, status);
    }
    // else kprintf("[NVMe] Read LBA %ld OK\n", lba);
}

void nvme_write_sector(uint64_t lba, void *buf) {
    nvme_sq_entry_t cmd = {0};
    cmd.opcode = NVME_CMD_WRITE;
    cmd.nsid = 1;
    cmd.prp1 = v2p(buf);
    
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = 0; 
    
    int status = nvme_submit_io_cmd(&cmd);
    if (status != 0) {
        kprintf("[NVMe] Write Error LBA %ld Status %x\n", lba, status);
    }
}


void nvme_init(pci_device_t *dev) {
    if (!dev) return;

    if (dev->prog_if != PCI_PROGIF_NVME) {
        kprintf("[NVMe] Found NVMe controller but weird ProgIF=0x%x\n", dev->prog_if);
        return;
    }
    
    kprintf("[NVMe] Initializing Controller at %02x:%02x.%d\n", dev->bus, dev->slot, dev->func);
    
    pci_enable_bus_master(dev);
    pci_enable_memory(dev);
    
    // Get BAR0
    uint64_t bar_phys = pci_get_bar_address(dev, 0, NULL);
    nvme.base = (uintptr_t)p2v(bar_phys);
    
    // Read Capabilities
    uint64_t cap = nvme_read_reg64(NVME_REG_CAP);
    uint32_t to = (cap >> 24) & 0xFF; // Timeout (500ms units)
    uint32_t dstrd = (cap >> 32) & 0xF; // Doorbell Stride (2 ^ (2 + DSTRD))
    
    nvme.db_stride = 4 << dstrd;
    
    kprintf("[NVMe] Cap=0x%lx Timeout=%d units DSTRD=%d\n", cap, to, dstrd);
    
    // Disable Controller
    uint32_t cc = nvme_read_reg32(NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        nvme_write_reg32(NVME_REG_CC, cc & ~NVME_CC_EN);
        // Wait for RDY to clear
        while (nvme_read_reg32(NVME_REG_CSTS) & NVME_CSTS_RDY);
    }
    
    // Allocate Admin Queues
    uint64_t asq_phys = (uint64_t)pmm_alloc_page();
    uint64_t acq_phys = (uint64_t)pmm_alloc_page();
    
    nvme.admin_sq = (nvme_sq_entry_t *)p2v(asq_phys);
    nvme.admin_cq = (nvme_cq_entry_t *)p2v(acq_phys);
    
    memset(nvme.admin_sq, 0, 4096);
    memset(nvme.admin_cq, 0, 4096);
    
    nvme.admin_sq_phys = asq_phys;
    nvme.admin_cq_phys = acq_phys;
    
    // Set AQA (Admin Queue Attributes) - 4096 entries max (0-based 12 bits)
    // We use a small queue size for admin (e.g., 64 entries)
    // AQA: ASQS (bits 0:11) = 63, ACQS (bits 16:27) = 63
    uint32_t aqa = (63 << 16) | 63;
    nvme_write_reg32(NVME_REG_AQA, aqa);
    
    // Set ASQ and ACQ addresses
    nvme_write_reg64(NVME_REG_ASQ, nvme.admin_sq_phys);
    nvme_write_reg64(NVME_REG_ACQ, nvme.admin_cq_phys);
    
    // Enable Controller
    // IOCQES = 4 (16 bytes), IOSQES = 6 (64 bytes), EN = 1
    uint32_t new_cc = NVME_CC_EN | NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16;
    nvme_write_reg32(NVME_REG_CC, new_cc);
    
    // Wait for RDY
    while (!(nvme_read_reg32(NVME_REG_CSTS) & NVME_CSTS_RDY));
    
    kprintf("[NVMe] Controller Enabled and Ready.\n");
    
    // Identify Controller
    nvme.admin_cq_phase = 1; // Expect 1 for first completion? No, init to 1.
    nvme_identify();
    
    // Create IO Queues
    nvme_setup_io_queues();
}
