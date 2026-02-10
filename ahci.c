#include "ahci.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "io.h"
#include "semaphore.h"

// Helper to convert physical to virtual (HHDM)
static inline void *p2v(uint64_t phys) {
    return (void *)(phys + vmm_get_hhdm_offset());
}

// Global AHCI state
static ahci_hba_mem_t *abar = NULL;
static int active_port = -1;
static ahci_cmd_header_t *cmd_header_virt[32];
static ahci_cmd_table_t *cmd_table_virt[32];
static semaphore_t port_sem[32];

// Interrupt Synchronization
static volatile int cmd_complete = 0;

// Interrupt Handler (called from IDT)
void ahci_isr(void) {
    if (!abar) return;

    // Check Global IS
    uint32_t is = abar->is;
    if (is == 0) return;

    // Acknowledge Global IS
    abar->is = is;

    // Check Active Port
    if (active_port != -1 && (is & (1 << active_port))) {
        ahci_port_regs_t *port = &abar->ports[active_port];
        uint32_t pis = port->is;
        
        // Clear Port IS
        port->is = pis;
        
        // Check for errors
        if (pis & (1 << 30)) { // TFES - Task File Error Status
            kprintf("[AHCI] ISR: Disk Error (IS=0x%x)\n", pis);
            cmd_complete = -1; // Error
            sem_post(&port_sem[active_port]);
        } else if (pis & (1 << 5)) { // DPS - Descriptor Processed
             // This fires for every PRD. We want completion.
        }
        
        // We typically look for D2H Register FIS (bit 0) or Set Device Bits (bit 2)
        if (pis & ((1 << 0) | (1 << 2) | (1 << 5))) {
            cmd_complete = 1;
            sem_post(&port_sem[active_port]);
        }
    }
}

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
    // Wait until CR is clear (with timeout)
    int limit = 1000000;
    while ((port->cmd & AHCI_CMD_CR) && limit > 0) {
        limit--;
        __asm__ volatile("pause");
    }
    if (limit == 0) {
        kprintf("[AHCI] Warning: start_cmd: CR stuck (CMD=0x%x)\n", port->cmd);
    }

    port->cmd |= AHCI_CMD_FRE; // Set FRE
    port->cmd |= AHCI_CMD_ST;  // Set ST
}

// Stop command engine
static void stop_cmd(ahci_port_regs_t *port) {
    // 1. Clear ST (Start)
    port->cmd &= ~AHCI_CMD_ST;

    // 2. Wait for CR (Command List Running) and FR (FIS Receive Running) to clear
    int limit = 1000000;
    while (port->cmd & (AHCI_CMD_CR | AHCI_CMD_FR)) {
        limit--;
        if (limit == 0) break;
    }

    // 3. If stuck, try Command List Override (CLO) if supported
    if (port->cmd & (AHCI_CMD_CR | AHCI_CMD_FR)) {
        if (abar->cap & AHCI_CAP_SCLO) {
            kprintf("[AHCI] Port hung, attempting CLO...\n");
            port->cmd |= AHCI_CMD_CLO;
            
            // Wait for CLO to clear
            limit = 1000000;
            while (port->cmd & AHCI_CMD_CLO) {
                limit--;
                if (limit == 0) break;
            }
        }
    }

    // 4. Clear FRE (FIS Receive Enable)
    port->cmd &= ~AHCI_CMD_FRE;

    // 5. Final wait
    limit = 1000000;
    while (port->cmd & (AHCI_CMD_CR | AHCI_CMD_FR)) {
        limit--;
        if (limit == 0) {
            kprintf("[AHCI] Warning: Stop CMD timed out (CMD=0x%x)\n", port->cmd);
            break;
        }
    }
}

// Port Reset (COMRESET)
static void port_reset(ahci_port_regs_t *port) {
    // 1. Set DET=1 (Initialize)
    uint32_t sctl = port->sctl;
    sctl = (sctl & 0xFFFFFFF0) | 1;
    port->sctl = sctl;

    // 2. Wait > 1ms (spin)
    for (volatile int i = 0; i < 100000; i++);

    // 3. Set DET=0 (No action)
    sctl = (sctl & 0xFFFFFFF0);
    port->sctl = sctl;

    // 4. Wait for DET=3 (Device present)
    int limit = 1000000;
    while ((port->ssts & 0xF) != 3) {
        limit--;
        if (limit == 0) {
            kprintf("[AHCI] Warning: Port reset failed (no device?)\n");
            return;
        }
    }
    
    // Clear error register
    port->serr = 0xFFFFFFFF;
}

// Configure a port
static void port_configure(int port_no) {
    ahci_port_regs_t *port = &abar->ports[port_no];

    // Spec recommendation: Stop engine BEFORE resetting
    kprintf("[AHCI] Port %d: Stopping CMD...\n", port_no);
    stop_cmd(port);

    kprintf("[AHCI] Port %d: Resetting...\n", port_no);
    port_reset(port);
    
    // Stop again to be sure after reset
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

    // Initialize semaphore for this port (count=0, ISR will post)
    sem_init(&port_sem[port_no], 0);

    // Start command engine
    start_cmd(port);

    // Wait for ST to be set
    int spin = 0;
    while (!(port->cmd & AHCI_CMD_ST) && spin < 1000000) spin++;
    if (spin >= 1000000) kprintf("[AHCI] Warning: Start CMD timed out\n");

    // Clear error status
    port->serr = 0xFFFFFFFF;
    
    // Enable Interrupts
    // DHRS (Bit 0) - D2H Register FIS
    // PSS (Bit 1) - PIO Setup FIS
    // DSS (Bit 2) - DMA Setup FIS
    // SDBS (Bit 3) - Set Device Bits FIS
    // UFS (Bit 4) - Unknown FIS
    // DPS (Bit 5) - Descriptor Processed
    // TFES (Bit 30) - Task File Error Status
    port->ie = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 30);
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
    
    // Perform HBA Reset
    kprintf("[AHCI] Resetting HBA...\n");
    abar->ghc |= AHCI_GHC_HR;
    int limit = 1000000;
    while ((abar->ghc & AHCI_GHC_HR) && limit > 0) {
        limit--;
    }
    if (limit == 0) {
        kprintf("[AHCI] Warning: HBA Reset timed out\n");
    }

    // Enable AHCI mode
    abar->ghc |= AHCI_GHC_AE;
    
    // Enable MSI (Vector 46)
    if (pci_enable_msi(dev, 46, 0) == 0) {
        kprintf("[AHCI] MSI Enabled (Vector 46)\n");
    } else {
        kprintf("[AHCI] Warning: MSI Enable Failed, falling back to polling/Legacy IRQ\n");
    }

    // Enable Global Interrupts in GHC
    abar->ghc |= AHCI_GHC_IE;

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
    int spin = 0;
    while ((port->tfd & (0x80 | 0x08)) && spin < 1000000) {
        spin++;
    }
    if (spin >= 1000000) {
        kprintf("[AHCI] Read Error: Port hung (BSY/DRQ)\n");
        return -1;
    }
    
    // Reset Completion Flag
    cmd_complete = 0;

    // Drain any stale semaphore counts
    while (sem_try_wait(&port_sem[active_port]));

    port->ci = (1 << slot);

    // Wait for interrupt (or timeout via polling fallback)
    // Try semaphore-based wait first, fall back to polling if MSI failed
    int timeout = 10000000;
    while (1) {
        if (cmd_complete == 1) break;
        if (cmd_complete == -1) return -1;

        // Check hardware directly (fallback if MSI failed)
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            kprintf("[AHCI] Read Error (IS=0x%x)\n", port->is);
            return -1;
        }

        timeout--;
        if (timeout == 0) {
            kprintf("[AHCI] Read Timeout (cmd_complete=%d, ci=0x%x, is=0x%x)\n", cmd_complete, port->ci, port->is);
            return -1;
        }

        __asm__ volatile("pause");
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

    // Issue command
    int spin = 0;
    while ((port->tfd & (0x80 | 0x08)) && spin < 1000000) {
        spin++;
    }
    if (spin >= 1000000) {
        kprintf("[AHCI] Write Error: Port hung (BSY/DRQ)\n");
        return -1;
    }
    
    // Reset Completion Flag
    cmd_complete = 0;
    
    // Ensure semaphore is zero
    while(sem_try_wait(&port_sem[active_port])); 

    port->ci = (1 << slot);

    // Wait for interrupt (Blocking)
    sem_wait(&port_sem[active_port]);

    if (cmd_complete == -1) return -1;
    if (port->is & (1 << 30)) return -1;

    return 0;
}
