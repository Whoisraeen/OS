#include "idt.h"
#include "e1000.h"
#include "pci.h"
#include "console.h"
#include "serial.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "ioapic.h"

static e1000_state_t e1000;

static uint32_t e1000_read_reg(uint16_t offset) {
    return *(volatile uint32_t *)(e1000.mmio_base + offset);
}

static void e1000_write_reg(uint16_t offset, uint32_t val) {
    *(volatile uint32_t *)(e1000.mmio_base + offset) = val;
}

static void e1000_detect_eeprom(void) {
    uint32_t val = 0;
    e1000_write_reg(E1000_EERD, 1);
    for (int i = 0; i < 1000; i++) {
        val = e1000_read_reg(E1000_EERD);
        if (val & (1 << 4)) break; // Done bit
    }
}

static uint16_t e1000_read_eeprom(uint8_t addr) {
    uint32_t tmp = 0;
    if (e1000.pci_dev->device_id == E1000_DEV_I217) {
        e1000_write_reg(E1000_EERD, (1) | ((uint32_t)(addr) << 8));
    } else {
        e1000_write_reg(E1000_EERD, (1) | ((uint32_t)(addr) << 8));
    }
    
    while (!((tmp = e1000_read_reg(E1000_EERD)) & (1 << 4)));
    return (uint16_t)((tmp >> 16) & 0xFFFF);
}

static void e1000_read_mac(void) {
    if (e1000_read_reg(E1000_STATUS) & 0x80000) {
        // Load from EEPROM
        uint16_t w;
        w = e1000_read_eeprom(0);
        e1000.mac[0] = w & 0xFF;
        e1000.mac[1] = w >> 8;
        w = e1000_read_eeprom(1);
        e1000.mac[2] = w & 0xFF;
        e1000.mac[3] = w >> 8;
        w = e1000_read_eeprom(2);
        e1000.mac[4] = w & 0xFF;
        e1000.mac[5] = w >> 8;
    } else {
        // Load from RA registers
        uint32_t ral = e1000_read_reg(E1000_RA);
        uint32_t rah = e1000_read_reg(E1000_RA + 4);
        e1000.mac[0] = ral & 0xFF;
        e1000.mac[1] = (ral >> 8) & 0xFF;
        e1000.mac[2] = (ral >> 16) & 0xFF;
        e1000.mac[3] = (ral >> 24) & 0xFF;
        e1000.mac[4] = rah & 0xFF;
        e1000.mac[5] = (rah >> 8) & 0xFF;
    }
    kprintf("[E1000] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        e1000.mac[0], e1000.mac[1], e1000.mac[2],
        e1000.mac[3], e1000.mac[4], e1000.mac[5]);
}

void e1000_isr(void) {
    if (!e1000.mmio_base) return;
    
    uint32_t icr = e1000_read_reg(E1000_ICR);
    if (icr & E1000_ICR_RXT0) {
        // Handle Received Packets
        while ((e1000.rx_descs[e1000.rx_cur].status & 1)) {
            uint16_t len = e1000.rx_descs[e1000.rx_cur].length;
            uint8_t *buf = e1000.rx_buffers[e1000.rx_cur];
            
            if (e1000.rx_callback) {
                e1000.rx_callback(buf, len);
            }
            
            e1000.rx_descs[e1000.rx_cur].status = 0;
            e1000_write_reg(E1000_RDT, e1000.rx_cur); // Advance tail
            e1000.rx_cur = (e1000.rx_cur + 1) % E1000_NUM_RX_DESC;
        }
    }
}

void e1000_set_rx_callback(void (*callback)(const void *data, uint16_t len)) {
    e1000.rx_callback = callback;
}

uint8_t *e1000_get_mac(void) {
    return e1000.mac;
}

void e1000_init(void) {
    // Find device
    e1000.pci_dev = pci_find_device(INTEL_VENDOR_ID, E1000_DEV_82540EM);
    if (!e1000.pci_dev) {
        e1000.pci_dev = pci_find_device(INTEL_VENDOR_ID, E1000_DEV_82545EM);
        if (!e1000.pci_dev) return;
    }
    
    // Enable Bus Master
    pci_enable_bus_master(e1000.pci_dev);
    
    // Map MMIO
    uint32_t bar0_size;
    uint64_t bar0_phys = pci_get_bar_address(e1000.pci_dev, 0, &bar0_size);
    e1000.mmio_base = (uint8_t *)(bar0_phys + vmm_get_hhdm_offset());
    
    kprintf("[E1000] Found device at %02x:%02x.%d, MMIO at 0x%lx\n",
        e1000.pci_dev->bus, e1000.pci_dev->slot, e1000.pci_dev->func, bar0_phys);
        
    e1000_detect_eeprom();
    e1000_read_mac();
    
    // Reset
    e1000_write_reg(E1000_CTRL, E1000_CTRL_RST);
    // Wait?
    
    // Register ISR for MSI (Vector 47)
    // Note: If we use Legacy, we'll register that instead.
    
    // Enable MSI (Vector 47)
    // FORCE DISABLED to debug Vector 0 panic
    if (0 && pci_enable_msi(e1000.pci_dev, 47, 0) == 0) {
        kprintf("[E1000] MSI Enabled (Vector 47)\n");
        irq_register_handler(47, e1000_isr);
    } else {
        kprintf("[E1000] MSI Failed. Falling back to Legacy IRQ %d\n", e1000.pci_dev->irq_line);
        
        // Disable Legacy Interrupt Disable bit (ensure it's clear)
        // pci_enable_bus_master already enables IO/MEM/Master.
        // We assume INTx is enabled by default.
        
        uint8_t irq = e1000.pci_dev->irq_line;
        uint8_t vector = 32 + irq;
        
        // Map IOAPIC
        // #include "ioapic.h" -> Moved to top
        // Active Low? Edge/Level?
        // PCI interrupts are usually Level Triggered, Active Low.
        // IOAPIC_LEVEL | IOAPIC_ACTIVE_LOW
        // But QEMU might just work with defaults. 
        // Let's try matching standard ISA first or just unmasking.
        
        // GSI = irq. Vector = 32+irq. Dest=0. Flags=Level|ActiveLow?
        ioapic_route_irq(irq, vector, 0, 0); // Try defaults (Edge/High) first? Or just Unmask?
        ioapic_unmask(irq);
        
        irq_register_handler(vector, e1000_isr);
    }
    
    // Link Up
    e1000_write_reg(E1000_CTRL, e1000_read_reg(E1000_CTRL) | E1000_CTRL_SLU);
    
    // Init RX
    uint8_t *rx_ptr = (uint8_t *)pmm_alloc_pages(1); // 4KB
    e1000.rx_descs = (e1000_rx_desc_t *)((uint64_t)rx_ptr + vmm_get_hhdm_offset());
    memset(e1000.rx_descs, 0, 4096);
    
    // Buffers
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint8_t *buf = (uint8_t *)pmm_alloc_pages(1); // 4KB
        e1000.rx_descs[i].addr = (uint64_t)buf; // Physical
        e1000.rx_descs[i].status = 0;
        e1000.rx_buffers[i] = (uint8_t *)((uint64_t)buf + vmm_get_hhdm_offset()); // Virtual
    }
    
    e1000_write_reg(E1000_RDBAL, (uint64_t)rx_ptr & 0xFFFFFFFF);
    e1000_write_reg(E1000_RDBAH, (uint64_t)rx_ptr >> 32);
    e1000_write_reg(E1000_RDLEN, E1000_NUM_RX_DESC * 16);
    e1000_write_reg(E1000_RDH, 0);
    e1000_write_reg(E1000_RDT, E1000_NUM_RX_DESC - 1);
    e1000.rx_cur = 0;

    // Init TX
    uint8_t *tx_ptr = (uint8_t *)pmm_alloc_pages(1);
    e1000.tx_descs = (e1000_tx_desc_t *)((uint64_t)tx_ptr + vmm_get_hhdm_offset());
    memset(e1000.tx_descs, 0, 4096);
    
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
         uint8_t *buf = (uint8_t *)pmm_alloc_pages(1);
         e1000.tx_descs[i].addr = (uint64_t)buf;
         e1000.tx_descs[i].cmd = 0;
         e1000.tx_descs[i].status = 1; // Done
         e1000.tx_buffers[i] = (uint8_t *)((uint64_t)buf + vmm_get_hhdm_offset());
    }
    
    e1000_write_reg(E1000_TDBAL, (uint64_t)tx_ptr & 0xFFFFFFFF);
    e1000_write_reg(E1000_TDBAH, (uint64_t)tx_ptr >> 32);
    e1000_write_reg(E1000_TDLEN, E1000_NUM_TX_DESC * 16);
    e1000_write_reg(E1000_TDH, 0);
    e1000_write_reg(E1000_TDT, 0);
    e1000.tx_cur = 0;
    
    // Enable Interrupts
    e1000_write_reg(E1000_IMS, E1000_ICR_RXT0 | E1000_ICR_LSC);

    // Enable RX
    e1000_write_reg(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
                                E1000_RCTL_MPE | E1000_RCTL_SECRC);
    // Enable TX (TCTL_CT=0x10, TCTL_COLD=0x40 are typical defaults)
    e1000_write_reg(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                                (0x10 << 4) | (0x40 << 12));
    e1000_write_reg(E1000_TIPG, 0x00702008); // Standard inter-packet gap
}

int e1000_send_packet(const void *data, uint16_t len) {
    uint16_t cur = e1000.tx_cur;
    volatile e1000_tx_desc_t *desc = &e1000.tx_descs[cur];
    
    // Copy data
    if (len > 2048) len = 2048;
    memcpy(e1000.tx_buffers[cur], data, len);
    
    desc->length = len;
    desc->cmd = (1 << 0) | (1 << 1) | (1 << 3); // EOP | IFCS | RS
    desc->status = 0;
    
    e1000.tx_cur = (cur + 1) % E1000_NUM_TX_DESC;
    e1000_write_reg(E1000_TDT, e1000.tx_cur);
    
    return 0;
}
