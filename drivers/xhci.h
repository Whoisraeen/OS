#ifndef __DRIVERS_XHCI_H__
#define __DRIVERS_XHCI_H__

#include <stdint.h>
#include "pci.h"

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
// Interrupter Register Set 0 starts at +0x20
#define XHCI_IMAN(n)            (0x20 + (n * 32))      // Interrupter Management
#define XHCI_IMOD(n)            (0x24 + (n * 32))      // Interrupter Moderation
#define XHCI_ERSTSZ(n)          (0x28 + (n * 32))      // Event Ring Segment Table Size
#define XHCI_ERSTBA(n)          (0x30 + (n * 32))      // Event Ring Segment Table Base Address
#define XHCI_ERDP(n)            (0x38 + (n * 32))      // Event Ring Dequeue Pointer

#define XHCI_IMAN_IP            (1 << 0) // Interrupt Pending
#define XHCI_IMAN_IE            (1 << 1) // Interrupt Enable

// Data Structures

// Transfer Request Block (TRB) - 16 bytes
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

void xhci_init(void);

#endif // __DRIVERS_XHCI_H__
