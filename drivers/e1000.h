#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include "pci.h"

// Intel Vendor ID
#define INTEL_VENDOR_ID 0x8086

// E1000 Device IDs (Common in QEMU/VMware)
#define E1000_DEV_82540EM 0x100E
#define E1000_DEV_82545EM 0x100F
#define E1000_DEV_82543GC 0x1004
#define E1000_DEV_I217    0x153A

// Register Offsets
#define E1000_CTRL      0x0000  // Device Control
#define E1000_STATUS    0x0008  // Device Status
#define E1000_EERD      0x0014  // EEPROM Read
#define E1000_ICR       0x00C0  // Interrupt Cause Read
#define E1000_ITR       0x00C4  // Interrupt Throttling Rate
#define E1000_ICS       0x00C8  // Interrupt Cause Set
#define E1000_IMS       0x00D0  // Interrupt Mask Set
#define E1000_IMC       0x00D8  // Interrupt Mask Clear
#define E1000_RCTL      0x0100  // Receive Control
#define E1000_TCTL      0x0400  // Transmit Control
#define E1000_TIPG      0x0410  // Transmit IPG
#define E1000_RDBAL     0x2800  // RX Descriptor Base Address Low
#define E1000_RDBAH     0x2804  // RX Descriptor Base Address High
#define E1000_RDLEN     0x2808  // RX Descriptor Length
#define E1000_RDH       0x2810  // RX Descriptor Head
#define E1000_RDT       0x2818  // RX Descriptor Tail
#define E1000_TDBAL     0x3800  // TX Descriptor Base Address Low
#define E1000_TDBAH     0x3804  // TX Descriptor Base Address High
#define E1000_TDLEN     0x3808  // TX Descriptor Length
#define E1000_TDH       0x3810  // TX Descriptor Head
#define E1000_TDT       0x3818  // TX Descriptor Tail
#define E1000_MTA       0x5200  // Multicast Table Array
#define E1000_RA        0x5400  // Receive Address (MAC Address)

// Control Register Bitmasks
#define E1000_CTRL_SLU  (1 << 6)  // Set Link Up
#define E1000_CTRL_RST  (1 << 26) // Device Reset

// RCTL Bitmasks
#define E1000_RCTL_EN   (1 << 1)  // Receiver Enable
#define E1000_RCTL_SBP  (1 << 2)  // Store Bad Packets
#define E1000_RCTL_UPE  (1 << 3)  // Unicast Promiscuous Enabled
#define E1000_RCTL_MPE  (1 << 4)  // Multicast Promiscuous Enabled
#define E1000_RCTL_LPE  (1 << 5)  // Long Packet Enable
#define E1000_RCTL_BAM  (1 << 15) // Broadcast Accept Mode
#define E1000_RCTL_SECRC (1 << 26) // Strip Ethernet CRC

// TCTL Bitmasks
#define E1000_TCTL_EN   (1 << 1)  // Transmit Enable
#define E1000_TCTL_PSP  (1 << 3)  // Pad Short Packets
#define E1000_TCTL_CT   (0x0F << 4) // Collision Threshold
#define E1000_TCTL_COLD (0x3F << 12) // Collision Distance
#define E1000_TCTL_RTLC (1 << 24) // Re-transmit on Late Collision

// Interrupt Bitmasks
#define E1000_ICR_TXDW  (1 << 0)  // Transmit Descriptor Written Back
#define E1000_ICR_TXQE  (1 << 1)  // Transmit Queue Empty
#define E1000_ICR_LSC   (1 << 2)  // Link Status Change
#define E1000_ICR_RXSEQ (1 << 3)  // Receive Sequence Error
#define E1000_ICR_RXT0  (1 << 7)  // Receiver Timer Interrupt

// Descriptor definitions
#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 32

typedef struct {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint16_t checksum;
    volatile uint8_t status;
    volatile uint8_t errors;
    volatile uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint8_t cso;
    volatile uint8_t cmd;
    volatile uint8_t status;
    volatile uint8_t css;
    volatile uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

// E1000 Driver State
typedef struct {
    pci_device_t *pci_dev;
    uint8_t *mmio_base;
    uint8_t mac[6];
    
    e1000_rx_desc_t *rx_descs;
    e1000_tx_desc_t *tx_descs;
    
    uint8_t *rx_buffers[E1000_NUM_RX_DESC];
    uint8_t *tx_buffers[E1000_NUM_TX_DESC];
    
    uint16_t rx_cur;
    uint16_t tx_cur;
    
    int irq;

    // Receive Callback
    void (*rx_callback)(const void *data, uint16_t len);
} e1000_state_t;

// API
void e1000_init(void);
int e1000_send_packet(const void *data, uint16_t len);
void e1000_set_rx_callback(void (*callback)(const void *data, uint16_t len));
uint8_t *e1000_get_mac(void);

#endif
