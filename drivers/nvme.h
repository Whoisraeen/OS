#ifndef _DRIVERS_NVME_H
#define _DRIVERS_NVME_H

#include <stdint.h>
#include "pci.h"

// PCI Class/Subclass
#define PCI_CLASS_STORAGE       0x01
#define PCI_SUBCLASS_NVME       0x08
#define PCI_PROGIF_NVME         0x02

// Registers (Offsets from BAR0)
#define NVME_REG_CAP            0x0000 // Controller Capabilities
#define NVME_REG_VS             0x0008 // Version
#define NVME_REG_INTMS          0x000C // Interrupt Mask Set
#define NVME_REG_INTMC          0x0010 // Interrupt Mask Clear
#define NVME_REG_CC             0x0014 // Controller Configuration
#define NVME_REG_CSTS           0x001C // Controller Status
#define NVME_REG_NSSR           0x0020 // NVM Subsystem Reset
#define NVME_REG_AQA            0x0024 // Admin Queue Attributes
#define NVME_REG_ASQ            0x0028 // Admin Submission Queue Base Address
#define NVME_REG_ACQ            0x0030 // Admin Completion Queue Base Address
#define NVME_REG_CMBMSC         0x0050 // Controller Memory Buffer Location
#define NVME_REG_CMBSZ          0x0054 // Controller Memory Buffer Size

// Doorbell Configuration
#define NVME_DOORBELL_OFFSET    0x1000

// Controller Configuration (CC) Bits
#define NVME_CC_EN              (1 << 0)
#define NVME_CC_CSS_NVM         (0 << 4)
#define NVME_CC_MPS_4K          (0 << 7) // Page Size 4KB (2 ^ (12 + 0))
#define NVME_CC_AMS_RR          (0 << 11) // Round Robin
#define NVME_CC_SHN_NONE        (0 << 14)
#define NVME_CC_IOSQES_64       (6 << 16) // 64 bytes (2^6)
#define NVME_CC_IOCQES_16       (4 << 20) // 16 bytes (2^4)

// Controller Status (CSTS) Bits
#define NVME_CSTS_RDY           (1 << 0)
#define NVME_CSTS_CFS           (1 << 1) // Controller Fatal Status

// Admin Commands
#define NVME_ADMIN_DELETE_SQ    0x00
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_DELETE_CQ    0x04
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0A

// NVM I/O Commands
#define NVME_CMD_FLUSH          0x00
#define NVME_CMD_WRITE          0x01
#define NVME_CMD_READ           0x02

// Data Structures

// Submission Queue Entry (64 bytes)
typedef struct {
    uint8_t opcode;
    uint8_t flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sq_entry_t;

// Completion Queue Entry (16 bytes)
typedef struct {
    uint32_t cdw0;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status; // Phase Tag (P) is bit 0 of status
} __attribute__((packed)) nvme_cq_entry_t;

// Identify Controller Data (Simplified)
typedef struct {
    uint16_t vid;
    uint16_t ssvid;
    char sn[20];
    char mn[40];
    char fr[8];
    uint8_t rab;
    uint8_t ieee[3];
    uint8_t cmic;
    uint8_t mdts;
    // ... rest is 4096 bytes total
    uint8_t rsvd[4096 - 78];
} __attribute__((packed)) nvme_identify_ctrl_t;

// NVMe Driver State
typedef struct {
    uintptr_t base;        // Virtual MMIO Base
    uint32_t db_stride;    // Doorbell Stride (bytes)
    
    // Command Ring (Virt)
    nvme_sq_entry_t *admin_sq;
    uint64_t admin_sq_phys;
    nvme_cq_entry_t *admin_cq;
    uint64_t admin_cq_phys;
    
    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    uint8_t admin_cq_phase;
    
    // IO Queue 1
    nvme_sq_entry_t *io_sq;
    uint64_t io_sq_phys;
    nvme_cq_entry_t *io_cq;
    uint64_t io_cq_phys;
    
    uint16_t io_sq_tail;
    uint16_t io_cq_head;
    uint8_t io_cq_phase;
    
    // Namespace info
    uint32_t ns_count;
    
} nvme_t;

void nvme_init(pci_device_t *dev);

#endif // _DRIVERS_NVME_H
