 #ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include <stddef.h>
#include "pci.h"
#include "driver.h"

// AHCI Generic Host Control Registers
#define AHCI_CAP       0x00
#define AHCI_GHC       0x04
#define AHCI_IS        0x08
#define AHCI_PI        0x0C
#define AHCI_VS        0x10

// GHC Bits
#define AHCI_GHC_AE    (1 << 31)  // AHCI Enable
#define AHCI_GHC_MRSM  (1 << 2)   // MSI Revert to Single Message
#define AHCI_GHC_IE    (1 << 1)   // Interrupt Enable
#define AHCI_GHC_HR    (1 << 0)   // HBA Reset

// Port Registers (0x100 + port * 0x80)
#define AHCI_PORT_CLB  0x00       // Command List Base
#define AHCI_PORT_CLBU 0x04       // Command List Base Upper
#define AHCI_PORT_FB   0x08       // FIS Base
#define AHCI_PORT_FBU  0x0C       // FIS Base Upper
#define AHCI_PORT_IS   0x10       // Interrupt Status
#define AHCI_PORT_IE   0x14       // Interrupt Enable
#define AHCI_PORT_CMD  0x18       // Command and Status
#define AHCI_PORT_TFD  0x20       // Task File Data
#define AHCI_PORT_SIG  0x24       // Signature
#define AHCI_PORT_SSTS 0x28       // SATA Status
#define AHCI_PORT_SCTL 0x2C       // SATA Control
#define AHCI_PORT_SERR 0x30       // SATA Error
#define AHCI_PORT_SACT 0x34       // SATA Active
#define AHCI_PORT_CI   0x38       // Command Issue

// Port Command Bits
#define AHCI_CMD_ST    (1 << 0)   // Start
#define AHCI_CMD_SUD   (1 << 1)   // Spin-Up Device
#define AHCI_CMD_POD   (1 << 2)   // Power On Device
#define AHCI_CMD_FRE   (1 << 4)   // FIS Receive Enable
#define AHCI_CMD_FR    (1 << 14)  // FIS Receive Running
#define AHCI_CMD_CR    (1 << 15)  // Command List Running

// Signature Types
#define AHCI_SIG_ATA   0x00000101
#define AHCI_SIG_ATAPI 0xEB140101
#define AHCI_SIG_SEMB  0xC33C0101
#define AHCI_SIG_PM    0x96690101

// FIS Types
typedef enum {
    FIS_TYPE_REG_H2D   = 0x27, // Register Host to Device
    FIS_TYPE_REG_D2H   = 0x34, // Register Device to Host
    FIS_TYPE_DMA_ACT   = 0x39, // DMA Activate
    FIS_TYPE_DMA_SETUP = 0x41, // DMA Setup
    FIS_TYPE_DATA      = 0x46, // Data
    FIS_TYPE_BIST      = 0x58, // BIST Activate
    FIS_TYPE_PIO_SETUP = 0x5F, // PIO Setup
    FIS_TYPE_DEV_BITS  = 0xA1  // Set Device Bits
} fis_type_t;

// Register Host to Device FIS (H2D)
typedef struct {
    uint8_t  fis_type;      // 0x27
    uint8_t  pm_port : 4;   // Port multiplier
    uint8_t  rsv0 : 3;      // Reserved
    uint8_t  c : 1;         // Command/Control (1=Command)
    uint8_t  command;       // Command register
    uint8_t  feature_l;     // Feature register (low)
    uint8_t  lba0;          // LBA low register, 7:0
    uint8_t  lba1;          // LBA mid register, 15:8
    uint8_t  lba2;          // LBA high register, 23:16
    uint8_t  device;        // Device register
    uint8_t  lba3;          // LBA register, 31:24
    uint8_t  lba4;          // LBA register, 39:32
    uint8_t  lba5;          // LBA register, 47:40
    uint8_t  feature_h;     // Feature register (high)
    uint8_t  count_l;       // Count register (low)
    uint8_t  count_h;       // Count register (high)
    uint8_t  icc;           // Isochronous command completion
    uint8_t  control;       // Control register
    uint8_t  rsv1[4];       // Reserved
} __attribute__((packed)) fis_reg_h2d_t;

// Register Device to Host FIS (D2H)
typedef struct {
    uint8_t  fis_type;      // 0x34
    uint8_t  pm_port : 4;
    uint8_t  rsv0 : 2;
    uint8_t  i : 1;         // Interrupt bit
    uint8_t  rsv1 : 1;
    uint8_t  status;        // Status register
    uint8_t  error;         // Error register
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  rsv2;
    uint8_t  count_l;
    uint8_t  count_h;
    uint8_t  rsv3[2];
    uint8_t  rsv4[4];
} __attribute__((packed)) fis_reg_d2h_t;

// Command Header
typedef struct {
    uint8_t  cfl : 5;       // Command FIS length in DWORDS (usually 5)
    uint8_t  a : 1;         // ATAPI
    uint8_t  w : 1;         // Write (1) / Read (0)
    uint8_t  p : 1;         // Prefetchable
    uint8_t  r : 1;         // Reset
    uint8_t  b : 1;         // BIST
    uint8_t  c : 1;         // Clear busy upon R_OK
    uint8_t  rsv0 : 1;
    uint8_t  pmp : 4;       // Port multiplier port
    uint16_t prdtl;         // Physical region descriptor table length
    volatile uint32_t prdbc;// Physical region descriptor byte count
    uint32_t ctba;          // Command table descriptor base address
    uint32_t ctbau;         // Command table descriptor base address upper
    uint32_t rsv1[4];
} __attribute__((packed)) ahci_cmd_header_t;

// Physical Region Descriptor Table Entry
typedef struct {
    uint32_t dba;           // Data base address
    uint32_t dbau;          // Data base address upper
    uint32_t rsv0;
    uint32_t dbc : 22;      // Data byte count (size - 1)
    uint32_t rsv1 : 9;
    uint32_t i : 1;         // Interrupt on completion
} __attribute__((packed)) ahci_prdt_entry_t;

// Command Table
typedef struct {
    uint8_t  cfis[64];      // Command FIS
    uint8_t  acmd[16];      // ATAPI command
    uint8_t  rsv[48];       // Reserved
    ahci_prdt_entry_t prdt[1]; // PRDT entries (we allocate more dynamically if needed, usually 1 page)
} __attribute__((packed)) ahci_cmd_table_t;

// HBA Memory Structure (The whole register map)
typedef struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} __attribute__((packed)) ahci_port_regs_t;

typedef struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    ahci_port_regs_t ports[32];
} __attribute__((packed)) ahci_hba_mem_t;

// Function prototypes
void ahci_init(void);

// Read/Write sectors (sector = 512 bytes)
int ahci_read(uint64_t lba, uint32_t count, uint8_t *buffer);
int ahci_write(uint64_t lba, uint32_t count, const uint8_t *buffer);

#endif
