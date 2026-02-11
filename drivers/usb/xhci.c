// XHCI Driver with inline header definitions
// Bypassing header include issues

#include <stdint.h>
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "string.h"
#include "io.h"
#include "idt.h"
#include "heap.h"
#include "serial.h"
#include "drivers/usb/usb.h"

// --- Definitions from drivers/xhci.h ---

// PCI Class/Subclass
#define PCI_CLASS_SERIAL        0x0C
#define PCI_SUBCLASS_USB        0x03
#define PCI_PROGIF_XHCI         0x30

// Capability Registers (Offsets from BAR0)
#define XHCI_CAPLENGTH          0x00 // 1 byte
#define XHCI_HCIVERSION         0x02 // 2 bytes
#define XHCI_HCSPARAMS1         0x04 // 4 bytes
#define XHCI_HCSPARAMS2         0x08 // 4 bytes
#define XHCI_HCSPARAMS3         0x0C // 4 bytes
#define XHCI_HCCPARAMS1         0x10 // 4 bytes
#define XHCI_DBOFF              0x14 // 4 bytes
#define XHCI_RTSOFF             0x18 // 4 bytes
#define XHCI_HCCPARAMS2         0x1C // 4 bytes

// Operational Registers (Offsets from Base + CAPLENGTH)
#define XHCI_USBCMD             0x00 // 4 bytes
#define XHCI_USBSTS             0x04 // 4 bytes
#define XHCI_PAGESIZE           0x08 // 4 bytes
#define XHCI_DNCTRL             0x14 // 4 bytes
#define XHCI_CRCR               0x18 // 8 bytes (Cmd Ring Control)
#define XHCI_DCBAAP             0x30 // 8 bytes (Dev Ctx Base Addr Array Ptr)
#define XHCI_CONFIG             0x38 // 4 bytes

// USBCMD Bits
#define XHCI_CMD_RS             (1 << 0) // Run/Stop
#define XHCI_CMD_HCRST          (1 << 1) // Host Controller Reset
#define XHCI_CMD_INTE           (1 << 2) // Interrupter Enable
#define XHCI_CMD_HSEE           (1 << 3) // Host System Error Enable

// USBSTS Bits
#define XHCI_STS_HCH            (1 << 0) // HCHalted
#define XHCI_STS_HSE            (1 << 2) // Host System Error
#define XHCI_STS_EINT           (1 << 3) // Event Interrupt
#define XHCI_STS_PCD            (1 << 4) // Port Change Detect
#define XHCI_STS_CNR            (1 << 11) // Controller Not Ready

// Port Status and Control Register (PORTSC) - Array at Base + 0x400
#define XHCI_PORTSC_CCS         (1 << 0) // Current Connect Status
#define XHCI_PORTSC_PED         (1 << 1) // Port Enabled/Disabled
#define XHCI_PORTSC_OCA         (1 << 3) // Over-current Active
#define XHCI_PORTSC_PR          (1 << 4) // Port Reset
#define XHCI_PORTSC_PP          (1 << 9) // Port Power
#define XHCI_PORTSC_PLS_MASK    (0xF << 5)
#define XHCI_PORTSC_SPEED_MASK  (0xF << 10)
#define XHCI_PORTSC_CSC         (1 << 17) // Connect Status Change
#define XHCI_PORTSC_PEDC        (1 << 18) // Port Enable/Disable Change
#define XHCI_PORTSC_PRC         (1 << 21) // Port Reset Change

// Runtime Registers (Offsets from Base + RTSOFF)
#define XHCI_IMAN(n)            (0x20 + (n * 32))      // Interrupter Management
#define XHCI_IMOD(n)            (0x24 + (n * 32))      // Interrupter Moderation
#define XHCI_ERSTSZ(n)          (0x28 + (n * 32))      // Event Ring Segment Table Size
#define XHCI_ERSTBA(n)          (0x30 + (n * 32))      // Event Ring Segment Table Base Address
#define XHCI_ERDP(n)            (0x38 + (n * 32))      // Event Ring Dequeue Pointer

#define XHCI_IMAN_IP            (1 << 0) // Interrupt Pending
#define XHCI_IMAN_IE            (1 << 1) // Interrupt Enable

// Data Structures

// Transfer Request Block (TRB)
typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

// TRB Types
#define TRB_NORMAL              1
#define TRB_SETUP_STAGE         2
#define TRB_DATA_STAGE          3
#define TRB_STATUS_STAGE        4
#define TRB_LINK                6
#define TRB_ENABLE_SLOT         9
#define TRB_DISABLE_SLOT        10
#define TRB_ADDRESS_DEVICE      11
#define TRB_CONFIGURE_EP        12
#define TRB_EVAL_CONTEXT        13
#define TRB_RESET_EP            14
#define TRB_NO_OP               23
#define TRB_TRANSFER_EVENT      32
#define TRB_CMD_COMPLETION      33
#define TRB_PORT_SC_EVENT       34

// Event Ring Segment Table Entry
typedef struct {
    uint64_t base_addr;
    uint32_t size;      // Number of TRBs
    uint32_t rsvd;
} __attribute__((packed)) xhci_erst_entry_t;

// Contexts
// Slot Context (32 bytes)
typedef struct {
    uint32_t f1;
    uint32_t f2;
    uint32_t f3;
    uint32_t f4;
    uint32_t rsvd[4];
} __attribute__((packed)) xhci_slot_ctx_t;

// Endpoint Context (32 bytes)
typedef struct {
    uint32_t f1;
    uint32_t f2;
    uint64_t tr_dequeue_ptr;
    uint32_t avg_tr_len;
    uint32_t rsvd[3];
} __attribute__((packed)) xhci_ep_ctx_t;

// Main XHCI Driver State
typedef struct {
    uintptr_t base;        // Virtual Base Address
    uintptr_t oper_base;   // Operational Base
    uintptr_t runtime_base;// Runtime Base
    uintptr_t db_base;     // Doorbell Base
    
    uint8_t cap_len;       // Capability Length
    uint32_t page_size;
    
    // Limits
    uint8_t max_slots;
    uint16_t max_intrs;
    uint8_t max_ports;
    
    // Memory Structures
    uint64_t *dcbaa;       // Device Context Base Addr Array (Virt)
    uint64_t dcbaa_phys;   // Physical
    
    xhci_trb_t *cmd_ring;  // Command Ring (Virt)
    uint64_t cmd_ring_phys;// Physical
    uint32_t cmd_ring_index;
    uint8_t cmd_ring_cycle;
    
    xhci_erst_entry_t *erst;
    uint64_t erst_phys;
    
    xhci_trb_t *event_ring;
    uint64_t event_ring_phys;
    uint32_t event_ring_index;
    uint8_t event_ring_cycle;
    
} xhci_t;

void xhci_init(pci_device_t *dev);

// --- End Definitions ---

static xhci_t xhci;

// Helper to convert physical to virtual (HHDM)
static inline void *p2v(uint64_t phys) {
    return (void *)(phys + vmm_get_hhdm_offset());
}

static inline uint64_t v2p(void *virt) {
    return (uint64_t)virt - vmm_get_hhdm_offset();
}

// MMIO Access Helpers
static inline uint32_t xhci_cap_read32(uint32_t reg) {
    return *(volatile uint32_t *)(xhci.base + reg);
}

static inline uint32_t xhci_op_read32(uint32_t reg) {
    return *(volatile uint32_t *)(xhci.oper_base + reg);
}

static inline void xhci_op_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(xhci.oper_base + reg) = val;
}

static inline void xhci_op_write64(uint32_t reg, uint64_t val) {
    *(volatile uint32_t *)(xhci.oper_base + reg) = (uint32_t)val;
    *(volatile uint32_t *)(xhci.oper_base + reg + 4) = (uint32_t)(val >> 32);
}

static inline uint32_t xhci_rt_read32(uint32_t reg) {
    return *(volatile uint32_t *)(xhci.runtime_base + reg);
}

static inline void xhci_rt_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(xhci.runtime_base + reg) = val;
}

static inline void xhci_rt_write64(uint32_t reg, uint64_t val) {
    *(volatile uint32_t *)(xhci.runtime_base + reg) = (uint32_t)val;
    *(volatile uint32_t *)(xhci.runtime_base + reg + 4) = (uint32_t)(val >> 32);
}

static inline void xhci_db_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(xhci.db_base + reg) = val;
}

// Interrupt Handler
void xhci_isr(void) {
    uint32_t status = xhci_op_read32(XHCI_USBSTS);
    
    // Acknowledge interrupt
    xhci_op_write32(XHCI_USBSTS, status | XHCI_STS_EINT);
    
    // Clear Iman IP (Interrupt Pending) in Interrupter 0
    uint32_t iman = xhci_rt_read32(XHCI_IMAN(0));
    xhci_rt_write32(XHCI_IMAN(0), iman | XHCI_IMAN_IP);
    
    kprintf("[XHCI] ISR: Status=0x%x\n", status);
    
    // Process Event Ring here...
}

static void xhci_reset(void) {
    // 1. Stop Controller
    uint32_t cmd = xhci_op_read32(XHCI_USBCMD);
    cmd &= ~XHCI_CMD_RS; // Clear Run/Stop bit
    xhci_op_write32(XHCI_USBCMD, cmd);
    
    // Wait for HCHalted
    while (!(xhci_op_read32(XHCI_USBSTS) & XHCI_STS_HCH));
    
    // 2. Reset Controller
    cmd = xhci_op_read32(XHCI_USBCMD);
    cmd |= XHCI_CMD_HCRST;
    xhci_op_write32(XHCI_USBCMD, cmd);
    
    // Wait for reset to complete
    while (xhci_op_read32(XHCI_USBCMD) & XHCI_CMD_HCRST);
    
    // Wait for Controller Not Ready to clear
    while (xhci_op_read32(XHCI_USBSTS) & XHCI_STS_CNR);
    
    kprintf("[XHCI] Reset Complete.\n");
}

// Ring Doorbell
static void xhci_ring_doorbell(uint8_t slot, uint8_t target) {
    xhci_db_write32(slot * 4, target); // Doorbell register is array of 32-bit values
}

// Send Command TRB
static void xhci_send_command(xhci_trb_t *trb) {
    // Copy TRB to Command Ring
    xhci_trb_t *dest = &xhci.cmd_ring[xhci.cmd_ring_index];
    memcpy(dest, trb, sizeof(xhci_trb_t));
    
    // Set Cycle Bit
    if (xhci.cmd_ring_cycle) {
        dest->control |= 1;
    } else {
        dest->control &= ~1;
    }
    
    // Advance Index
    xhci.cmd_ring_index++;
    if (xhci.cmd_ring_index >= 256) { // Ring size
        // Link TRB handling (simplified: just wrap for now, should use Link TRB)
        xhci.cmd_ring_index = 0;
        xhci.cmd_ring_cycle = !xhci.cmd_ring_cycle;
        // NOTE: Real implementation needs Link TRB at end of ring!
        // But for <256 commands during init, this is ok.
    }
    
    // Ring Doorbell 0 (Host Controller Command)
    xhci_ring_doorbell(0, 0);
}

// Enable Slot
static uint8_t __attribute__((unused)) xhci_enable_slot(void) {
    xhci_trb_t cmd = {0};
    cmd.control = (TRB_ENABLE_SLOT << 10);
    
    xhci_send_command(&cmd);
    
    // In a real OS, we would wait for Command Completion Event on Event Ring.
    // For now, we assume it works and return slot 1 (hack).
    // TODO: Implement Event Ring Polling/Wait
    return 1;
}

// Address Device
static void __attribute__((unused)) xhci_address_device(uint8_t slot_id, uint8_t root_port_id) {
    // 1. Allocate Input Context
    // 2. Initialize Input Context (Enable Slot Context + EP0 Context)
    // 3. Send Address Device Command
    
    // Placeholder implementation
    kprintf("[XHCI] Address Device Slot=%d Port=%d\n", slot_id, root_port_id);
}

// Handle Port Status Change
static void xhci_handle_port_connect(int port_id) {
    kprintf("[XHCI] Port %d Connected. Resetting...\n", port_id);
    
    uint32_t portsc_reg = 0x400 + (port_id - 1) * 0x10;
    uint32_t portsc = xhci_op_read32(portsc_reg);
    
    // Reset Port
    xhci_op_write32(portsc_reg, portsc | XHCI_PORTSC_PR);
    
    // Wait for PRC (Port Reset Change) is handled in ISR usually, 
    // but for simple poll we might want to check it.
}

// Poll ports for connection (initial scan)
static void xhci_scan_ports(void) {
    for (int i = 1; i <= xhci.max_ports; i++) {
        uint32_t portsc = xhci_op_read32(0x400 + (i - 1) * 0x10);
        if (portsc & XHCI_PORTSC_CCS) {
            xhci_handle_port_connect(i);
        }
    }
}

void xhci_init(pci_device_t *dev) {
    if (!dev) return;
    
    // Check ProgIF if checking specifically for XHCI
    if (dev->class_code == PCI_CLASS_SERIAL && dev->subclass == PCI_SUBCLASS_USB && dev->prog_if != PCI_PROGIF_XHCI) {
        kprintf("[XHCI] Device is USB but not XHCI (ProgIF=0x%x)\n", dev->prog_if);
        return;
    }
    
    kprintf("[XHCI] Initializing Controller at %02x:%02x.%d\n", dev->bus, dev->slot, dev->func);
    
    pci_enable_bus_master(dev);
    pci_enable_memory(dev);
    
    // Get BAR0 (MMIO Base)
    uint64_t bar_phys = pci_get_bar_address(dev, 0, NULL);
    
    // Map memory
    xhci.base = (uintptr_t)p2v(bar_phys);
    
    // Read Capabilities
    xhci.cap_len = xhci_cap_read32(XHCI_CAPLENGTH) & 0xFF;
    xhci.oper_base = xhci.base + xhci.cap_len;
    
    uint32_t rtsoff = xhci_cap_read32(XHCI_RTSOFF);
    uint32_t dboff = xhci_cap_read32(XHCI_DBOFF);
    
    xhci.runtime_base = xhci.base + rtsoff;
    xhci.db_base = xhci.base + dboff;
    
    // Read Structural Parameters 1
    uint32_t hcsparams1 = xhci_cap_read32(XHCI_HCSPARAMS1);
    xhci.max_slots = hcsparams1 & 0xFF;
    xhci.max_intrs = (hcsparams1 >> 8) & 0x7FF;
    xhci.max_ports = (hcsparams1 >> 24) & 0xFF;
    
    kprintf("[XHCI] Init: MaxSlots=%d, MaxIntrs=%d, MaxPorts=%d\n", 
            xhci.max_slots, xhci.max_intrs, xhci.max_ports);
            
    // Reset Host Controller
    xhci_reset();
    
    // Configure Max Device Slots
    uint32_t config = xhci_op_read32(XHCI_CONFIG);
    config &= ~0xFF;
    config |= xhci.max_slots;
    xhci_op_write32(XHCI_CONFIG, config);
    
    // Allocate DCBAA (Device Context Base Address Array)
    // Size = (MaxSlots + 1) * 64-bit pointers
    // Must be 64-byte aligned
    uint64_t dcbaa_phys = (uint64_t)pmm_alloc_page(); // Allocates 4KB, enough for >500 slots
    xhci.dcbaa_phys = dcbaa_phys;
    xhci.dcbaa = (uint64_t *)p2v(dcbaa_phys);
    memset(xhci.dcbaa, 0, 4096);
    
    xhci_op_write64(XHCI_DCBAAP, xhci.dcbaa_phys);
    
    // Allocate Command Ring (One segment, 4KB)
    uint64_t cr_phys = (uint64_t)pmm_alloc_page();
    xhci.cmd_ring_phys = cr_phys;
    xhci.cmd_ring = (xhci_trb_t *)p2v(cr_phys);
    memset(xhci.cmd_ring, 0, 4096);
    xhci.cmd_ring_cycle = 1;
    xhci.cmd_ring_index = 0;
    
    // Set CRCR (Command Ring Control Register)
    // Bit 0: RCS (Ring Cycle State) = 1 (Consumer expects 1)
    xhci_op_write64(XHCI_CRCR, xhci.cmd_ring_phys | 1);
    
    // Allocate Event Ring Segment Table (ERST) - 1 Entry for now
    uint64_t erst_phys = (uint64_t)pmm_alloc_page();
    xhci.erst_phys = erst_phys;
    xhci.erst = (xhci_erst_entry_t *)p2v(erst_phys);
    memset(xhci.erst, 0, 4096);
    
    // Allocate Event Ring (One segment, 4KB = 256 TRBs)
    uint64_t er_phys = (uint64_t)pmm_alloc_page();
    xhci.event_ring_phys = er_phys;
    xhci.event_ring = (xhci_trb_t *)p2v(er_phys);
    memset(xhci.event_ring, 0, 4096);
    xhci.event_ring_cycle = 1;
    xhci.event_ring_index = 0;
    
    // Setup ERST Entry
    xhci.erst[0].base_addr = xhci.event_ring_phys;
    xhci.erst[0].size = 256; // Number of TRBs
    
    // Write ERST to Interrupter 0
    xhci_rt_write32(XHCI_ERSTSZ(0), 1); // 1 Segment
    xhci_rt_write64(XHCI_ERSTBA(0), xhci.erst_phys);
    
    // Write Dequeue Pointer
    xhci_rt_write64(XHCI_ERDP(0), xhci.event_ring_phys | (1 << 3)); // Bit 3 is EHB (Busy) - Clear it
    
    // Enable Interrupter 0
    xhci_rt_write32(XHCI_IMOD(0), 1000); // Moderation interval (approx 250us)
    xhci_rt_write32(XHCI_IMAN(0), XHCI_IMAN_IE | XHCI_IMAN_IP); // Enable + Clear Pending
    
    // Register MSI Vector
    // We'll trust the PCI subsystem to give us a vector.
    // Let's assume vector 47 for USB (since AHCI is 46)
    if (pci_enable_msi(dev, 47, 0) == 0) {
        irq_register_handler(47, xhci_isr);
        kprintf("[XHCI] MSI Enabled (Vector 47)\n");
    }
    
    // Start Controller
    uint32_t cmd = xhci_op_read32(XHCI_USBCMD);
    cmd |= (XHCI_CMD_RS | XHCI_CMD_INTE | XHCI_CMD_HSEE);
    xhci_op_write32(XHCI_USBCMD, cmd);
    
    kprintf("[XHCI] Controller Started.\n");
    
    // Initial Port Scan
    xhci_scan_ports();
}
