#include "ahci.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "io.h"

// Helper to convert physical to virtual (HHDM)
static inline void *p2v(uint64_t phys) {
    return (void *)(phys + vmm_get_hhdm_offset());
}

// Global AHCI state
static ahci_hba_mem_t *abar = NULL;
static int active_port = -1;
static ahci_cmd_header_t *cmd_header_virt[32];
static ahci_cmd_table_t *cmd_table_virt[32];

// Check device type
static int check_type(ahci_port_regs_t *port) {
    uint32_t ssts = port->ssts;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != 3) return 0; // Not present
    if (ipm != 1) return 0; // Not active state

    switch (port->sig) {
        case AHCI_SIG_ATAPI: return 2; // SATAPI
        case AHCI_SIG_SEMB:  return 3; // SEMB
        case AHCI_SIG_PM:    return 4; // PM
        default:             return 1; // SATA
    }
}

// Start command engine
static void start_cmd(ahci_port_regs_t *port) {
    while (port->cmd & AHCI_CMD_CR); // Wait until CR is clear

    port->cmd |= AHCI_CMD_FRE; // Set FRE
    port->cmd |= AHCI_CMD_ST;  // Set ST
}

// Stop command engine
static void stop_cmd(ahci_port_regs_t *port) {
    port->cmd &= ~AHCI_CMD_ST; // Clear ST
    port->cmd &= ~AHCI_CMD_FRE; // Clear FRE

    int limit = 1000000;
    while (port->cmd & (AHCI_CMD_FR | AHCI_CMD_CR)) {
        limit--;
        if (limit == 0) {
            kprintf("[AHCI] Warning: Stop CMD timed out (CMD=0x%x)\n", port->cmd);
            break;
        }
    }
}

// Configure a port
static void port_configure(int port_no) {
    kprintf("[AHCI] Port %d: Stopping CMD...\n", port_no);
    ahci_port_regs_t *port = &abar->ports[port_no];
    stop_cmd(port);

    kprintf("[AHCI] Port %d: Allocating memory...\n", port_no);
    // Allocate Command List (1KB aligned)
    // We allocate a full page (4KB) to be safe and simple
    uint64_t cl_phys = (uint64_t)pmm_alloc_page();
    if (cl_phys == 0) {
        kprintf("[AHCI] PMM Alloc failed\n");
        return;
    }
    void *cl_virt = p2v(cl_phys);
    memset(cl_virt, 0, 4096);

    port->clb = (uint32_t)cl_phys;
    port->clbu = (uint32_t)(cl_phys >> 32);

    // Allocate FIS (256B aligned)
    uint64_t fb_phys = cl_phys + 1024;
    port->fb = (uint32_t)fb_phys;
    port->fbu = (uint32_t)(fb_phys >> 32);

    // Setup Command Header (Slot 0)
    cmd_header_virt[port_no] = (ahci_cmd_header_t *)cl_virt;
    ahci_cmd_header_t *hdr = &cmd_header_virt[port_no][0];
    
    hdr->cfl = sizeof(fis_reg_h2d_t) / 4; // 5 DWORDS
    hdr->w = 0;
    hdr->prdtl = 1; // 1 PRDT entry
    
    // Command Table
    uint64_t ct_phys = cl_phys + 1024 + 256;
    hdr->ctba = (uint32_t)ct_phys;
    hdr->ctbau = (uint32_t)(ct_phys >> 32);

    cmd_table_virt[port_no] = (ahci_cmd_table_t *)p2v(ct_phys);
    memset(cmd_table_virt[port_no], 0, sizeof(ahci_cmd_table_t));

    kprintf("[AHCI] Port %d: Starting CMD...\n", port_no);
    start_cmd(port);
}

void ahci_init(void) {
    kprintf("[AHCI] Searching for controller...\n");
    pci_device_t *dev = pci_find_device_by_class(0x01, 0x06); // SATA
    if (!dev) {
        kprintf("[AHCI] No SATA controller found.\n");
        return;
    }

    kprintf("[AHCI] Found controller at %02x:%02x.%d\n", dev->bus, dev->slot, dev->func);

    // Enable Bus Master and Memory
    pci_enable_bus_master(dev);
    pci_enable_memory(dev);

    // Get ABAR (BAR5)
    uint32_t bar5_size = 0;
    uint64_t bar5_phys = pci_get_bar_address(dev, 5, &bar5_size);
    if (bar5_phys == 0) {
        kprintf("[AHCI] Invalid BAR5.\n");
        return;
    }

    // Map ABAR
    abar = (ahci_hba_mem_t *)p2v(bar5_phys);
    
    // Check AHCI Enable
    if (!(abar->ghc & AHCI_GHC_AE)) {
        abar->ghc |= AHCI_GHC_AE;
    }

    kprintf("[AHCI] ABAR mapped at 0x%lx (phys 0x%lx)\n", (uint64_t)abar, bar5_phys);
    kprintf("[AHCI] Cap: 0x%x, PI: 0x%x, Ver: 0x%x\n", abar->cap, abar->pi, abar->vs);

    // Find valid port
    uint32_t pi = abar->pi;
    for (int i = 0; i < 32; i++) {
        if (pi & 1) {
            int type = check_type(&abar->ports[i]);
            if (type == 1) { // SATA
                kprintf("[AHCI] Port %d is SATA. Configuring...\n", i);
                port_configure(i);
                active_port = i;
                // For now, we only support one drive
                break;
            } else if (type == 2) {
                kprintf("[AHCI] Port %d is SATAPI (CD-ROM). Ignored.\n", i);
            }
        }
        pi >>= 1;
    }

    if (active_port == -1) {
        kprintf("[AHCI] No active SATA drive found.\n");
    } else {
        kprintf("[AHCI] Initialized. Active Port: %d\n", active_port);
    }
}

// Find a free command slot
static int find_cmd_slot(ahci_port_regs_t *port) {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & 1) == 0) return i;
        slots >>= 1;
    }
    return -1;
}

int ahci_read(uint64_t lba, uint32_t count, uint8_t *buffer) {
    if (active_port == -1) return -1;

    ahci_port_regs_t *port = &abar->ports[active_port];
    
    // Clear interrupts
    port->is = 0xFFFFFFFF;

    int slot = 0; // We forced slot 0 in config, but let's stick to it for simplicity
    
    ahci_cmd_header_t *hdr = &cmd_header_virt[active_port][slot];
    ahci_cmd_table_t *tbl = cmd_table_virt[active_port];

    // Setup Header
    hdr->cfl = sizeof(fis_reg_h2d_t) / 4;
    hdr->w = 0; // Read
    hdr->prdtl = 1; // 1 entry for now (assuming buffer is contiguous physically)

    // Setup FIS
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1; // Command
    fis->command = 0x25; // READ DMA EXT
    
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 1 << 6; // LBA mode
    
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    
    fis->count_l = (uint8_t)count;
    fis->count_h = (uint8_t)(count >> 8);

    // Setup PRDT
    // Buffer must be physical address!
    // We assume the buffer passed in is a virtual address in HHDM or kernel space.
    // If it's a user buffer, we'd need to walk page tables. 
    // For now, assume kernel buffer in HHDM.
    uint64_t buf_phys = (uint64_t)buffer - vmm_get_hhdm_offset();
    
    tbl->prdt[0].dba = (uint32_t)buf_phys;
    tbl->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    tbl->prdt[0].dbc = (count * 512) - 1; // 512 bytes per sector
    tbl->prdt[0].i = 1;

    // Issue command
    while (port->tfd & (0x80 | 0x08)); // Wait for BSY and DRQ to clear
    
    port->ci = (1 << slot);

    // Wait for completion
    while (1) {
        if ((port->ci & (1 << slot)) == 0) break; // Command cleared
        if (port->is & (1 << 30)) { // Error
            kprintf("[AHCI] Read Error (IS=0x%x, TFD=0x%x)\n", port->is, port->tfd);
            return -1;
        }
    }

    return 0;
}

int ahci_write(uint64_t lba, uint32_t count, const uint8_t *buffer) {
    if (active_port == -1) return -1;

    ahci_port_regs_t *port = &abar->ports[active_port];
    port->is = 0xFFFFFFFF;

    int slot = 0;
    ahci_cmd_header_t *hdr = &cmd_header_virt[active_port][slot];
    ahci_cmd_table_t *tbl = cmd_table_virt[active_port];

    hdr->cfl = sizeof(fis_reg_h2d_t) / 4;
    hdr->w = 1; // Write
    hdr->prdtl = 1;

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = 0x35; // WRITE DMA EXT
    
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 1 << 6;
    
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    
    fis->count_l = (uint8_t)count;
    fis->count_h = (uint8_t)(count >> 8);

    uint64_t buf_phys = (uint64_t)buffer - vmm_get_hhdm_offset();
    
    tbl->prdt[0].dba = (uint32_t)buf_phys;
    tbl->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    tbl->prdt[0].dbc = (count * 512) - 1;
    tbl->prdt[0].i = 1;

    while (port->tfd & (0x80 | 0x08));
    
    port->ci = (1 << slot);

    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            kprintf("[AHCI] Write Error (IS=0x%x)\n", port->is);
            return -1;
        }
    }

    return 0;
}
