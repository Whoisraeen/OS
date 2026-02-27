// XHCI Driver - Full USB HID Gamepad Support
#include <stdint.h>
#include <stdbool.h>
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
#include "drivers/usb/hid_gamepad.h"

// ── PCI identifiers ──────────────────────────────────────────────────────────
#define PCI_CLASS_SERIAL   0x0C
#define PCI_SUBCLASS_USB   0x03
#define PCI_PROGIF_XHCI    0x30

// ── Capability register offsets ──────────────────────────────────────────────
#define XHCI_CAPLENGTH   0x00
#define XHCI_HCIVERSION  0x02
#define XHCI_HCSPARAMS1  0x04
#define XHCI_HCSPARAMS2  0x08
#define XHCI_HCSPARAMS3  0x0C
#define XHCI_HCCPARAMS1  0x10
#define XHCI_DBOFF       0x14
#define XHCI_RTSOFF      0x18

// ── Operational register offsets ─────────────────────────────────────────────
#define XHCI_USBCMD  0x00
#define XHCI_USBSTS  0x04
#define XHCI_PAGESIZE 0x08
#define XHCI_DNCTRL  0x14
#define XHCI_CRCR    0x18
#define XHCI_DCBAAP  0x30
#define XHCI_CONFIG  0x38

// ── USBCMD bits ──────────────────────────────────────────────────────────────
#define XHCI_CMD_RS    (1u << 0)
#define XHCI_CMD_HCRST (1u << 1)
#define XHCI_CMD_INTE  (1u << 2)
#define XHCI_CMD_HSEE  (1u << 3)

// ── USBSTS bits ──────────────────────────────────────────────────────────────
#define XHCI_STS_HCH  (1u << 0)
#define XHCI_STS_HSE  (1u << 2)
#define XHCI_STS_EINT (1u << 3)
#define XHCI_STS_PCD  (1u << 4)
#define XHCI_STS_CNR  (1u << 11)

// ── Port Status/Control offsets ───────────────────────────────────────────────
#define XHCI_PORTSC_BASE  0x400
#define XHCI_PORTSC(n)    (XHCI_PORTSC_BASE + ((n)-1) * 0x10)
#define XHCI_PORTSC_CCS   (1u << 0)
#define XHCI_PORTSC_PED   (1u << 1)
#define XHCI_PORTSC_PR    (1u << 4)
#define XHCI_PORTSC_PP    (1u << 9)
#define XHCI_PORTSC_CSC   (1u << 17)
#define XHCI_PORTSC_PRC   (1u << 21)
// Speed field: bits 13:10
#define XHCI_PORTSC_SPEED(ps) (((ps) >> 10) & 0xF)
// Write-1-to-clear status change bits mask
#define XHCI_PORTSC_W1C   (0x00FE0000u)

// ── Runtime register helpers ──────────────────────────────────────────────────
#define XHCI_IMAN(n)   (0x20 + (n)*32)
#define XHCI_IMOD(n)   (0x24 + (n)*32)
#define XHCI_ERSTSZ(n) (0x28 + (n)*32)
#define XHCI_ERSTBA(n) (0x30 + (n)*32)
#define XHCI_ERDP(n)   (0x38 + (n)*32)
#define XHCI_IMAN_IP   (1u << 0)
#define XHCI_IMAN_IE   (1u << 1)

// ── TRB types ────────────────────────────────────────────────────────────────
#define TRB_NORMAL          1
#define TRB_SETUP_STAGE     2
#define TRB_DATA_STAGE      3
#define TRB_STATUS_STAGE    4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_TRANSFER_EVENT  32
#define TRB_CMD_COMPLETION  33
#define TRB_PORT_SC_EVENT   34

// TRB control field helpers
#define TRB_TYPE(t)    ((uint32_t)(t) << 10)
#define TRB_CYCLE      (1u << 0)
#define TRB_ENT        (1u << 1)   // Evaluate Next TRB
#define TRB_ISP        (1u << 2)   // Interrupt on Short Packet
#define TRB_NOSNOOP    (1u << 3)
#define TRB_CH         (1u << 4)   // Chain
#define TRB_IOC        (1u << 5)   // Interrupt on Completion
#define TRB_IDT        (1u << 6)   // Immediate Data (Setup TRB)
#define TRB_DIR_IN     (1u << 16)  // Data/Status stage direction

// Setup TRB Transfer Type (TRT) in bits 17:16
#define TRB_TRT_NO_DATA  (0u << 16)
#define TRB_TRT_OUT      (2u << 16)
#define TRB_TRT_IN       (3u << 16)

// Completion codes (in event TRB status bits 31:24)
#define CC_SUCCESS        1
#define CC_DATA_BUFFER    2
#define CC_BABBLE         3
#define CC_TRANSACTION    4
#define CC_TRB            5
#define CC_STALL          6
#define CC_SHORT_PACKET   13

// ── Data structures ───────────────────────────────────────────────────────────
typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

typedef struct {
    uint64_t base_addr;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed)) xhci_erst_entry_t;

// Transfer ring: 64 TRBs (63 usable + 1 Link TRB)
#define XHCI_RING_SIZE 64

typedef struct {
    xhci_trb_t  *trbs;       // virtual
    uint64_t     phys;
    uint32_t     enqueue;    // next TRB to write
    uint8_t      cycle;      // producer cycle bit
} xhci_ring_t;

// Per-device slot state
typedef struct {
    bool      active;
    uint8_t   slot_id;
    uint8_t   port_id;
    uint8_t   speed;         // 1=LS,2=FS,3=HS,4=SS

    // Input context (for Address Device / Configure EP commands)
    void     *input_ctx;
    uint64_t  input_ctx_phys;

    // Output device context
    void     *dev_ctx;
    uint64_t  dev_ctx_phys;

    // Control endpoint (EP0) transfer ring
    xhci_ring_t ep0;

    // Interrupt IN endpoint (for HID reports)
    xhci_ring_t intr;
    uint8_t      intr_dci;   // doorbell target (DCI of interrupt EP)
    uint16_t     intr_mps;   // max packet size

    // HID report buffer (physical + virtual)
    uint8_t  *report_buf;
    uint64_t  report_buf_phys;
    uint8_t   report_len;

    bool      is_hid_gamepad;
} xhci_slot_t;

// ── Global controller state ───────────────────────────────────────────────────
typedef struct {
    uintptr_t base;
    uintptr_t oper_base;
    uintptr_t runtime_base;
    uintptr_t db_base;

    uint8_t  cap_len;
    uint8_t  ctx_size;       // 32 or 64 bytes per context entry (CSZ bit)

    uint8_t  max_slots;
    uint16_t max_intrs;
    uint8_t  max_ports;

    uint64_t *dcbaa;
    uint64_t  dcbaa_phys;

    xhci_ring_t cmd_ring;

    xhci_erst_entry_t *erst;
    uint64_t           erst_phys;

    xhci_trb_t *event_ring;
    uint64_t    event_ring_phys;
    uint32_t    event_ring_index;
    uint8_t     event_ring_cycle;

    xhci_slot_t slots[32];   // support up to 32 devices

    // Last command completion info (filled by event processing)
    volatile uint64_t last_cmd_trb;
    volatile uint8_t  last_cmd_cc;
    volatile uint8_t  last_cmd_slot;
    volatile bool     cmd_done;

    // Last transfer completion info (per slot/DCI)
    volatile uint64_t last_xfer_trb;
    volatile uint8_t  last_xfer_cc;
    volatile uint8_t  last_xfer_slot;
    volatile uint8_t  last_xfer_dci;
    volatile int32_t  last_xfer_residual;
    volatile bool     xfer_done;
} xhci_t;

static xhci_t xhci;

// ── HHDM helpers ─────────────────────────────────────────────────────────────
static inline void *p2v(uint64_t phys) {
    return (void *)(phys + vmm_get_hhdm_offset());
}
static inline uint64_t v2p(void *virt) {
    return (uint64_t)virt - vmm_get_hhdm_offset();
}

// ── MMIO helpers ──────────────────────────────────────────────────────────────
static inline uint32_t cap_r32(uint32_t r)        { return *(volatile uint32_t *)(xhci.base + r); }
static inline uint32_t op_r32(uint32_t r)         { return *(volatile uint32_t *)(xhci.oper_base + r); }
static inline void     op_w32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(xhci.oper_base + r) = v; }
static inline void     op_w64(uint32_t r, uint64_t v) {
    *(volatile uint32_t *)(xhci.oper_base + r)     = (uint32_t)v;
    *(volatile uint32_t *)(xhci.oper_base + r + 4) = (uint32_t)(v >> 32);
}
static inline uint32_t rt_r32(uint32_t r)         { return *(volatile uint32_t *)(xhci.runtime_base + r); }
static inline void     rt_w32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(xhci.runtime_base + r) = v; }
static inline void     rt_w64(uint32_t r, uint64_t v) {
    *(volatile uint32_t *)(xhci.runtime_base + r)     = (uint32_t)v;
    *(volatile uint32_t *)(xhci.runtime_base + r + 4) = (uint32_t)(v >> 32);
}
static inline void db_w32(uint32_t slot, uint32_t target) {
    *(volatile uint32_t *)(xhci.db_base + slot * 4) = target;
}

// ── Ring helpers ──────────────────────────────────────────────────────────────
static bool xhci_ring_alloc(xhci_ring_t *ring) {
    uint64_t phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    if (!phys) return false;
    ring->trbs   = (xhci_trb_t *)p2v(phys);
    ring->phys   = phys;
    ring->enqueue = 0;
    ring->cycle   = 1;
    memset(ring->trbs, 0, 4096);

    // Install Link TRB at last slot pointing back to start
    xhci_trb_t *link = &ring->trbs[XHCI_RING_SIZE - 1];
    link->parameter = phys;
    link->status    = 0;
    link->control   = TRB_TYPE(TRB_LINK) | (1u << 1); // TC=1 (Toggle Cycle)
    return true;
}

// Enqueue a TRB on a ring; advances enqueue and handles Link TRB wrap.
// Returns physical address of the TRB that was written (for completion matching).
static uint64_t xhci_ring_enqueue(xhci_ring_t *ring, uint64_t param,
                                   uint32_t status, uint32_t control) {
    uint32_t idx = ring->enqueue;
    xhci_trb_t *trb = &ring->trbs[idx];

    trb->parameter = param;
    trb->status    = status;
    // Set cycle bit last, matching producer cycle
    trb->control   = (control & ~TRB_CYCLE) | (ring->cycle ? TRB_CYCLE : 0);

    uint64_t trb_phys = ring->phys + idx * sizeof(xhci_trb_t);

    ring->enqueue++;
    if (ring->enqueue >= XHCI_RING_SIZE - 1) {
        // Update Link TRB cycle bit and wrap
        xhci_trb_t *link = &ring->trbs[XHCI_RING_SIZE - 1];
        link->control ^= TRB_CYCLE; // toggle cycle on link TRB
        ring->enqueue  = 0;
        ring->cycle   ^= 1;
    }
    return trb_phys;
}

// ── Event ring processing ─────────────────────────────────────────────────────
// Returns true if any event was processed. Updates last_cmd_* / last_xfer_* globals.
static bool xhci_process_one_event(void) {
    xhci_trb_t *evt = &xhci.event_ring[xhci.event_ring_index];
    // Valid event if cycle bit matches expected
    if ((evt->control & TRB_CYCLE) != (xhci.event_ring_cycle ? TRB_CYCLE : 0))
        return false;

    uint32_t type = (evt->control >> 10) & 0x3F;
    uint8_t  cc   = (evt->status >> 24) & 0xFF;

    if (type == TRB_CMD_COMPLETION) {
        xhci.last_cmd_trb  = evt->parameter & ~0xFULL; // TRB pointer
        xhci.last_cmd_cc   = cc;
        xhci.last_cmd_slot = (evt->control >> 24) & 0xFF;
        xhci.cmd_done      = true;
    } else if (type == TRB_TRANSFER_EVENT) {
        xhci.last_xfer_trb      = evt->parameter & ~0xFULL;
        xhci.last_xfer_cc       = cc;
        xhci.last_xfer_slot     = (evt->control >> 24) & 0xFF;
        xhci.last_xfer_dci      = (evt->control >> 16) & 0x1F;
        xhci.last_xfer_residual = (int32_t)(evt->status & 0xFFFFFF);
        xhci.xfer_done          = true;

        // Route completed HID reports to the gamepad driver
        uint8_t slot = xhci.last_xfer_slot;
        if (slot > 0 && slot < 32 && xhci.slots[slot].is_hid_gamepad &&
            xhci.slots[slot].report_buf &&
            (cc == CC_SUCCESS || cc == CC_SHORT_PACKET)) {
            int rlen = (int)xhci.slots[slot].report_len - (int)xhci.last_xfer_residual;
            if (rlen > 0)
                hid_gamepad_handle_report(xhci.slots[slot].report_buf, rlen);
            // Re-queue the interrupt transfer for the next report
            xhci_ring_t *ir = &xhci.slots[slot].intr;
            xhci_ring_enqueue(ir,
                xhci.slots[slot].report_buf_phys,
                (uint32_t)xhci.slots[slot].report_len,
                TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
            db_w32(slot, xhci.slots[slot].intr_dci);
        }
    } else if (type == TRB_PORT_SC_EVENT) {
        uint8_t port = (evt->parameter >> 24) & 0xFF;
        kprintf("[XHCI] Port %d status change\n", port);
    }

    // Advance event ring dequeue
    xhci.event_ring_index++;
    if (xhci.event_ring_index >= 256) {
        xhci.event_ring_index = 0;
        xhci.event_ring_cycle ^= 1;
    }

    // Update ERDP to let controller know we consumed this event
    uint64_t erdp = xhci.event_ring_phys +
                    xhci.event_ring_index * sizeof(xhci_trb_t);
    rt_w64(XHCI_ERDP(0), erdp | (1u << 3)); // EHB=1 clears interrupt pending

    return true;
}

// Spin-poll until a command completion arrives. Returns completion code.
static uint8_t xhci_wait_cmd(uint64_t trb_phys) {
    xhci.cmd_done = false;
    for (int i = 0; i < 100000; i++) {
        while (xhci_process_one_event()) { /* drain */ }
        if (xhci.cmd_done && xhci.last_cmd_trb == trb_phys)
            return xhci.last_cmd_cc;
    }
    kprintf("[XHCI] TIMEOUT waiting for command completion\n");
    return 0xFF; // timeout
}

// Spin-poll until a transfer completion for (slot, dci). Returns CC.
static uint8_t xhci_wait_xfer(uint8_t slot, uint8_t dci) {
    xhci.xfer_done = false;
    for (int i = 0; i < 200000; i++) {
        while (xhci_process_one_event()) { /* drain */ }
        if (xhci.xfer_done &&
            xhci.last_xfer_slot == slot &&
            xhci.last_xfer_dci  == dci)
            return xhci.last_xfer_cc;
    }
    kprintf("[XHCI] TIMEOUT waiting for transfer slot=%d dci=%d\n", slot, dci);
    return 0xFF;
}

// ── Command ring ──────────────────────────────────────────────────────────────
static uint64_t xhci_send_cmd(uint64_t param, uint32_t status, uint32_t ctrl_no_cycle) {
    uint64_t phys = xhci_ring_enqueue(&xhci.cmd_ring, param, status, ctrl_no_cycle);
    db_w32(0, 0); // ring host controller doorbell
    return phys;
}

// ── Context helpers ───────────────────────────────────────────────────────────
// Byte offset into a context for a given entry index.
static inline uint32_t ctx_off(uint32_t entry) {
    return entry * xhci.ctx_size;
}

// Write a 32-bit field in input context
static inline void ictx_w32(xhci_slot_t *s, uint32_t entry, uint32_t dword, uint32_t val) {
    uint8_t *base = (uint8_t *)s->input_ctx;
    ((volatile uint32_t *)(base + ctx_off(entry)))[dword] = val;
}

// Read a 32-bit field from output device context
static inline uint32_t dctx_r32(xhci_slot_t *s, uint32_t entry, uint32_t dword) {
    uint8_t *base = (uint8_t *)s->dev_ctx;
    return ((volatile uint32_t *)(base + ctx_off(entry)))[dword];
}

// ── EP max packet size from speed ────────────────────────────────────────────
static uint16_t ep0_mps(uint8_t speed) {
    switch (speed) {
        case 3: return 64;   // High Speed
        case 4: return 512;  // Super Speed
        default: return 8;   // Full/Low Speed
    }
}

// ── DCI from USB endpoint address ────────────────────────────────────────────
// DCI = (ep_num * 2) + (ep_in ? 1 : 0)
static inline uint8_t ep_dci(uint8_t ep_addr) {
    uint8_t num = ep_addr & 0x0F;
    uint8_t dir = (ep_addr & 0x80) ? 1 : 0;
    return (uint8_t)(num * 2 + dir);
}

// ── Allocate slot contexts ────────────────────────────────────────────────────
static bool xhci_alloc_slot(xhci_slot_t *s) {
    // Input context: (max_dci+2) entries.  Allocate 1 page (plenty).
    uint64_t phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    if (!phys) return false;
    s->input_ctx      = p2v(phys);
    s->input_ctx_phys = phys;
    memset(s->input_ctx, 0, 4096);

    phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    if (!phys) return false;
    s->dev_ctx      = p2v(phys);
    s->dev_ctx_phys = phys;
    memset(s->dev_ctx, 0, 4096);

    if (!xhci_ring_alloc(&s->ep0)) return false;
    return true;
}

// ── Enable Slot (synchronous) ─────────────────────────────────────────────────
// Returns slot ID (1-based) or 0 on error.
static uint8_t xhci_enable_slot_sync(void) {
    uint64_t trb_phys = xhci_send_cmd(0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    uint8_t  cc       = xhci_wait_cmd(trb_phys);
    if (cc != CC_SUCCESS) {
        kprintf("[XHCI] Enable Slot failed: cc=%d\n", cc);
        return 0;
    }
    return xhci.last_cmd_slot;
}

// ── Address Device (synchronous) ──────────────────────────────────────────────
static bool xhci_address_device_sync(xhci_slot_t *s) {
    uint16_t mps = ep0_mps(s->speed);

    // Input Control Context (entry 0): A0=1 (Slot), A1=1 (EP0 DCI=1)
    ictx_w32(s, 0, 1, 0x3);  // Add flags: bit0=Slot, bit1=EP0

    // Slot Context (entry 1)
    uint32_t speed_field = (uint32_t)s->speed << 20;
    ictx_w32(s, 1, 0, speed_field | (1u << 27));   // Context Entries=1, Speed
    ictx_w32(s, 1, 1, (uint32_t)s->port_id << 16); // Root Hub Port Number

    // EP0 Context (entry 2): Control Bidir, EP Type=4
    uint32_t interval = (s->speed >= 3) ? 0 : 0; // 0 for FS/HS control
    ictx_w32(s, 2, 0, interval << 16);
    ictx_w32(s, 2, 1, (3u << 1) |          // Error Count=3
                       (4u << 3) |          // EP Type=4 (Control Bidir)
                       ((uint32_t)mps << 16));
    // TR Dequeue Pointer (entry 2, dwords 2-3) = ring phys | DCS=1
    uint64_t deq = s->ep0.phys | 1;
    ictx_w32(s, 2, 2, (uint32_t)deq);
    ictx_w32(s, 2, 3, (uint32_t)(deq >> 32));
    ictx_w32(s, 2, 4, 8); // Average TRB Length

    // Write device context pointer into DCBAA
    xhci.dcbaa[s->slot_id] = s->dev_ctx_phys;

    // Address Device command
    uint64_t trb_phys = xhci_send_cmd(s->input_ctx_phys,
                                       0,
                                       TRB_TYPE(TRB_ADDRESS_DEVICE) |
                                       ((uint32_t)s->slot_id << 24));
    uint8_t cc = xhci_wait_cmd(trb_phys);
    if (cc != CC_SUCCESS) {
        kprintf("[XHCI] Address Device failed: cc=%d slot=%d\n", cc, s->slot_id);
        return false;
    }
    kprintf("[XHCI] Slot %d addressed (speed=%d)\n", s->slot_id, s->speed);
    return true;
}

// ── Control transfer (synchronous) ───────────────────────────────────────────
// Returns bytes transferred (>=0) or negative on error.
static int xhci_control_transfer(xhci_slot_t *s,
                                  uint8_t  bm_req_type, uint8_t b_req,
                                  uint16_t w_value, uint16_t w_index,
                                  void *data, uint16_t len) {
    bool is_in = (bm_req_type & 0x80) != 0;

    // Build Setup TRB inline data (8 bytes = parameter field)
    uint64_t setup_param =
        (uint64_t)bm_req_type        |
        ((uint64_t)b_req       << 8) |
        ((uint64_t)w_value     << 16)|
        ((uint64_t)w_index     << 32)|
        ((uint64_t)len         << 48);

    uint32_t trt = (len == 0) ? TRB_TRT_NO_DATA : (is_in ? TRB_TRT_IN : TRB_TRT_OUT);

    // Setup Stage TRB
    xhci_ring_enqueue(&s->ep0, setup_param, 8,
                      TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | trt);

    uint64_t data_trb_phys = 0;
    uint64_t buf_phys = 0;
    if (len > 0 && data) {
        buf_phys = v2p(data);
        // Data Stage TRB
        data_trb_phys = xhci_ring_enqueue(&s->ep0, buf_phys,
                                           len,
                                           TRB_TYPE(TRB_DATA_STAGE) |
                                           (is_in ? TRB_DIR_IN : 0));
    }

    // Status Stage TRB (direction is opposite of data, or IN if no data)
    uint32_t status_dir = (is_in && len > 0) ? 0 : TRB_DIR_IN;
    uint64_t status_trb_phys = xhci_ring_enqueue(&s->ep0, 0, 0,
                                                   TRB_TYPE(TRB_STATUS_STAGE) |
                                                   status_dir | TRB_IOC);

    // Ring doorbell for EP0 (slot doorbell, target=1)
    db_w32(s->slot_id, 1);

    // Wait for transfer completion on EP0 (DCI=1)
    uint8_t cc = xhci_wait_xfer(s->slot_id, 1);
    (void)data_trb_phys;
    (void)status_trb_phys;

    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        return (int)len - (int)xhci.last_xfer_residual;
    }
    kprintf("[XHCI] Control transfer failed: cc=%d slot=%d\n", cc, s->slot_id);
    return -1;
}

// ── Configure Interrupt IN endpoint (synchronous) ────────────────────────────
static bool xhci_configure_intr_ep(xhci_slot_t *s, uint8_t ep_addr,
                                    uint16_t mps, uint8_t interval) {
    if (!xhci_ring_alloc(&s->intr)) return false;
    s->intr_dci = ep_dci(ep_addr);
    s->intr_mps = mps;

    // Re-init input context
    memset(s->input_ctx, 0, 4096);

    // Input Control Context: A0=Slot, A_dci = interrupt EP
    ictx_w32(s, 0, 1, (1u << 0) | (1u << s->intr_dci));

    // Slot Context: update Context Entries to cover our EP DCI
    uint32_t sc0 = dctx_r32(s, 1, 0); // read current from output context
    sc0 = (sc0 & ~(0x1Fu << 27)) | ((uint32_t)s->intr_dci << 27);
    ictx_w32(s, 1, 0, sc0);
    ictx_w32(s, 1, 1, dctx_r32(s, 1, 1));
    ictx_w32(s, 1, 2, dctx_r32(s, 1, 2));
    ictx_w32(s, 1, 3, dctx_r32(s, 1, 3));

    // Interrupt IN EP context (entry = intr_dci + 1 in input context)
    uint32_t ici = s->intr_dci + 1; // input context index
    // Interval: for HS: 2^(bInterval-1), already 0-based in spec field
    uint8_t  iv  = (interval > 0) ? (interval - 1) : 0;
    ictx_w32(s, ici, 0, (uint32_t)iv << 16);           // Interval
    ictx_w32(s, ici, 1, (3u << 1) |                    // Error Count=3
                         (7u << 3) |                    // EP Type=7 (Interrupt IN)
                         ((uint32_t)mps << 16));        // Max Packet Size
    uint64_t deq = s->intr.phys | 1;                   // DCS=1
    ictx_w32(s, ici, 2, (uint32_t)deq);
    ictx_w32(s, ici, 3, (uint32_t)(deq >> 32));
    ictx_w32(s, ici, 4, mps);                          // Average TRB Length

    uint64_t trb_phys = xhci_send_cmd(s->input_ctx_phys,
                                       0,
                                       TRB_TYPE(TRB_CONFIGURE_EP) |
                                       ((uint32_t)s->slot_id << 24));
    uint8_t cc = xhci_wait_cmd(trb_phys);
    if (cc != CC_SUCCESS) {
        kprintf("[XHCI] Configure EP failed: cc=%d\n", cc);
        return false;
    }
    kprintf("[XHCI] Slot %d interrupt EP 0x%02x configured (DCI=%d, MPS=%d)\n",
            s->slot_id, ep_addr, s->intr_dci, mps);
    return true;
}

// ── USB standard requests ─────────────────────────────────────────────────────
#define USB_BMRT_DEV_IN  0x80
#define USB_BMRT_IFACE   0x01
#define USB_BMRT_IFACE_IN 0x81

#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_BMRT_CLASS_IFACE      0x21
#define HID_SET_IDLE              0x0A
#define HID_SET_PROTOCOL          0x0B
#define HID_GET_REPORT_DESCRIPTOR 0x06  // via GET_DESCRIPTOR with type 0x22

static int usb_get_descriptor(xhci_slot_t *s, uint8_t type, uint8_t idx,
                               void *buf, uint16_t len) {
    return xhci_control_transfer(s,
        USB_BMRT_DEV_IN, USB_REQ_GET_DESCRIPTOR,
        (uint16_t)((uint16_t)type << 8 | idx), 0,
        buf, len);
}

static int usb_set_configuration(xhci_slot_t *s, uint8_t cfg) {
    return xhci_control_transfer(s, 0x00, USB_REQ_SET_CONFIGURATION, cfg, 0, NULL, 0);
}

static int hid_set_idle(xhci_slot_t *s, uint8_t iface) {
    return xhci_control_transfer(s, USB_BMRT_CLASS_IFACE, HID_SET_IDLE,
                                  0, iface, NULL, 0);
}

static int hid_set_protocol(xhci_slot_t *s, uint8_t iface, uint8_t proto) {
    // proto=1 = Report Protocol
    return xhci_control_transfer(s, USB_BMRT_CLASS_IFACE, HID_SET_PROTOCOL,
                                  proto, iface, NULL, 0);
}

// ── Full USB enumeration for one port ────────────────────────────────────────
static void xhci_enumerate_port(uint8_t port_id, uint8_t speed) {
    kprintf("[XHCI] Enumerating port %d speed=%d\n", port_id, speed);

    // 1. Enable Slot
    uint8_t slot_id = xhci_enable_slot_sync();
    if (!slot_id) return;

    xhci_slot_t *s = &xhci.slots[slot_id];
    memset(s, 0, sizeof(*s));
    s->slot_id = slot_id;
    s->port_id = port_id;
    s->speed   = speed;
    s->active  = true;

    if (!xhci_alloc_slot(s)) {
        kprintf("[XHCI] Out of memory for slot %d\n", slot_id);
        return;
    }

    // 2. Address Device
    if (!xhci_address_device_sync(s)) return;

    // Allocate a 4KB DMA buffer for descriptors
    uint64_t buf_phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    uint8_t *buf      = (uint8_t *)p2v(buf_phys);

    // 3. GET_DESCRIPTOR (Device, 8 bytes) — read bMaxPacketSize0
    memset(buf, 0, 8);
    if (usb_get_descriptor(s, 0x01, 0, buf, 8) < 0) {
        kprintf("[XHCI] Failed to get device descriptor\n");
        goto done;
    }
    // buf[7] = bMaxPacketSize0 — update EP0 MPS if needed (skip for now)

    // 4. GET_DESCRIPTOR (Device, full 18 bytes)
    memset(buf, 0, 18);
    if (usb_get_descriptor(s, 0x01, 0, buf, 18) < 8) {
        kprintf("[XHCI] Failed to get full device descriptor\n");
        goto done;
    }
    uint16_t vid = (uint16_t)(buf[8]  | (buf[9]  << 8));
    uint16_t pid = (uint16_t)(buf[10] | (buf[11] << 8));
    kprintf("[XHCI] Device VID=%04x PID=%04x\n", vid, pid);

    // 5. GET_DESCRIPTOR (Configuration, first 9 bytes)
    memset(buf, 0, 9);
    if (usb_get_descriptor(s, 0x02, 0, buf, 9) < 9) goto done;
    uint16_t total_len = (uint16_t)(buf[2] | (buf[3] << 8));
    if (total_len > 4096) total_len = 4096;

    // 6. GET_DESCRIPTOR (Configuration, full)
    memset(buf, 0, total_len);
    if (usb_get_descriptor(s, 0x02, 0, buf, total_len) < 4) goto done;

    // 7. SET_CONFIGURATION
    uint8_t cfg_val = buf[5];
    if (usb_set_configuration(s, cfg_val) < 0) goto done;

    // 8. Walk descriptors looking for HID gamepad interface + interrupt EP
    uint8_t  hid_iface     = 0xFF;
    uint8_t  intr_ep_addr  = 0;
    uint16_t intr_ep_mps   = 8;
    uint8_t  intr_interval = 10;
    bool     is_hid        = false;

    uint8_t *p   = buf;
    uint8_t *end = buf + total_len;

    while (p < end) {
        if (p + 2 > end) break;
        uint8_t dlen  = p[0];
        uint8_t dtype = p[1];
        if (dlen < 2) break;

        if (dtype == 0x04 && p + 9 <= end) {
            // Interface descriptor
            uint8_t iclass    = p[5];
            uint8_t isubclass = p[6];
            uint8_t iprotocol = p[7];
            // HID class=3, subclass=0/1, protocol=0(none)/1(kbd)/2(mouse)
            // Gamepad: class=3, subclass=0, protocol=0
            if (iclass == 0x03 && iprotocol == 0x00) {
                hid_iface = p[2]; // bInterfaceNumber
                is_hid = true;
                kprintf("[XHCI] HID interface %d (sub=%d proto=%d)\n",
                        hid_iface, isubclass, iprotocol);
            }
        } else if (dtype == 0x05 && p + 7 <= end && is_hid) {
            // Endpoint descriptor
            uint8_t ep_addr  = p[2];
            uint8_t ep_attr  = p[3]; // bits 1:0 = transfer type
            uint16_t ep_mps  = (uint16_t)(p[4] | (p[5] << 8)) & 0x7FF;
            uint8_t  ep_intv = p[6];
            // Interrupt IN endpoint
            if ((ep_attr & 0x03) == 0x03 && (ep_addr & 0x80)) {
                intr_ep_addr  = ep_addr;
                intr_ep_mps   = ep_mps;
                intr_interval = ep_intv;
                kprintf("[XHCI] Interrupt IN EP 0x%02x MPS=%d interval=%d\n",
                        intr_ep_addr, intr_ep_mps, intr_interval);
            }
        }
        p += dlen;
    }

    if (!is_hid || !intr_ep_addr) {
        kprintf("[XHCI] Not a HID gamepad, skipping.\n");
        goto done;
    }

    // 9. HID class setup
    hid_set_idle(s, hid_iface);
    hid_set_protocol(s, hid_iface, 1); // Report Protocol

    // 10. Configure interrupt IN endpoint
    if (!xhci_configure_intr_ep(s, intr_ep_addr, intr_ep_mps, intr_interval))
        goto done;

    // 11. Allocate report buffer and queue first interrupt transfer
    buf_phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    s->report_buf      = (uint8_t *)p2v(buf_phys);
    s->report_buf_phys = buf_phys;
    s->report_len      = (uint8_t)(intr_ep_mps < 64 ? intr_ep_mps : 64);
    memset(s->report_buf, 0, 4096);

    s->is_hid_gamepad = true;

    // Queue initial interrupt IN transfer
    xhci_ring_enqueue(&s->intr,
        s->report_buf_phys,
        s->report_len,
        TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    db_w32(s->slot_id, s->intr_dci);

    // Notify gamepad driver
    hid_gamepad_connected(slot_id, vid, pid);
    kprintf("[XHCI] Gamepad on slot %d ready.\n", slot_id);

done:
    // The buf page was used for descriptors; report_buf is separate.
    // We do NOT free buf_phys here intentionally (no page free implemented inline).
    (void)buf_phys;
}

// ── Port handling ─────────────────────────────────────────────────────────────
static void xhci_handle_port(uint8_t port_id) {
    uint32_t portsc = op_r32(XHCI_PORTSC(port_id));
    if (!(portsc & XHCI_PORTSC_CCS)) return;

    // Reset port
    op_w32(XHCI_PORTSC(port_id), (portsc & ~XHCI_PORTSC_W1C) | XHCI_PORTSC_PR);

    // Wait for Port Reset Change
    for (int i = 0; i < 50000; i++) {
        portsc = op_r32(XHCI_PORTSC(port_id));
        if (portsc & XHCI_PORTSC_PRC) break;
    }

    // Clear status change bits
    op_w32(XHCI_PORTSC(port_id), (portsc & ~XHCI_PORTSC_W1C) | XHCI_PORTSC_PRC);

    // Re-read for speed
    portsc = op_r32(XHCI_PORTSC(port_id));
    if (!(portsc & XHCI_PORTSC_CCS)) return; // disconnected during reset
    uint8_t speed = XHCI_PORTSC_SPEED(portsc);

    xhci_enumerate_port(port_id, speed);
}

// ── ISR ───────────────────────────────────────────────────────────────────────
void xhci_isr(void) {
    // Acknowledge
    op_w32(XHCI_USBSTS, op_r32(XHCI_USBSTS) | XHCI_STS_EINT);
    uint32_t iman = rt_r32(XHCI_IMAN(0));
    rt_w32(XHCI_IMAN(0), iman | XHCI_IMAN_IP);

    // Drain the event ring
    while (xhci_process_one_event()) { /* nothing */ }
}

// ── Reset ─────────────────────────────────────────────────────────────────────
static void xhci_reset(void) {
    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) & ~XHCI_CMD_RS);
    while (!(op_r32(XHCI_USBSTS) & XHCI_STS_HCH));
    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) | XHCI_CMD_HCRST);
    while (op_r32(XHCI_USBCMD) & XHCI_CMD_HCRST);
    while (op_r32(XHCI_USBSTS) & XHCI_STS_CNR);
    kprintf("[XHCI] Reset complete.\n");
}

// ── Init ──────────────────────────────────────────────────────────────────────
void xhci_init(pci_device_t *dev) {
    if (!dev) return;
    if (dev->class_code != PCI_CLASS_SERIAL ||
        dev->subclass   != PCI_SUBCLASS_USB  ||
        dev->prog_if    != PCI_PROGIF_XHCI) return;

    kprintf("[XHCI] Init %02x:%02x.%d\n", dev->bus, dev->slot, dev->func);
    pci_enable_bus_master(dev);
    pci_enable_memory(dev);

    uint64_t bar_phys = pci_get_bar_address(dev, 0, NULL);
    xhci.base         = (uintptr_t)p2v(bar_phys);
    xhci.cap_len      = cap_r32(XHCI_CAPLENGTH) & 0xFF;
    xhci.oper_base    = xhci.base + xhci.cap_len;

    uint32_t rtsoff = cap_r32(XHCI_RTSOFF);
    uint32_t dboff  = cap_r32(XHCI_DBOFF);
    xhci.runtime_base = xhci.base + rtsoff;
    xhci.db_base      = xhci.base + dboff;

    uint32_t hcsparams1 = cap_r32(XHCI_HCSPARAMS1);
    xhci.max_slots = hcsparams1 & 0xFF;
    xhci.max_intrs = (hcsparams1 >> 8)  & 0x7FF;
    xhci.max_ports = (hcsparams1 >> 24) & 0xFF;

    // Context size: CSZ bit in HCCPARAMS1 bit 2
    uint32_t hccparams1 = cap_r32(XHCI_HCCPARAMS1);
    xhci.ctx_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    kprintf("[XHCI] MaxSlots=%d MaxPorts=%d CtxSize=%d\n",
            xhci.max_slots, xhci.max_ports, xhci.ctx_size);

    xhci_reset();

    // Max slots
    op_w32(XHCI_CONFIG, (op_r32(XHCI_CONFIG) & ~0xFF) | xhci.max_slots);

    // DCBAA
    uint64_t dcbaa_phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    xhci.dcbaa      = (uint64_t *)p2v(dcbaa_phys);
    xhci.dcbaa_phys = dcbaa_phys;
    memset(xhci.dcbaa, 0, 4096);
    op_w64(XHCI_DCBAAP, dcbaa_phys);

    // Command Ring
    xhci_ring_alloc(&xhci.cmd_ring);
    op_w64(XHCI_CRCR, xhci.cmd_ring.phys | 1); // RCS=1

    // Event Ring Segment Table (1 segment)
    uint64_t erst_phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    xhci.erst      = (xhci_erst_entry_t *)p2v(erst_phys);
    xhci.erst_phys = erst_phys;
    memset(xhci.erst, 0, 4096);

    uint64_t er_phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    xhci.event_ring       = (xhci_trb_t *)p2v(er_phys);
    xhci.event_ring_phys  = er_phys;
    xhci.event_ring_index = 0;
    xhci.event_ring_cycle = 1;
    memset(xhci.event_ring, 0, 4096);

    xhci.erst[0].base_addr = er_phys;
    xhci.erst[0].size      = 256;

    rt_w32(XHCI_ERSTSZ(0), 1);
    rt_w64(XHCI_ERSTBA(0), erst_phys);
    rt_w64(XHCI_ERDP(0),   er_phys | (1u << 3));

    rt_w32(XHCI_IMOD(0), 0);
    rt_w32(XHCI_IMAN(0), XHCI_IMAN_IE | XHCI_IMAN_IP);

    // MSI
    if (pci_enable_msi(dev, 47, 0) == 0) {
        extern void irq_register_handler(int, void (*)(void));
        irq_register_handler(47, xhci_isr);
        kprintf("[XHCI] MSI vector 47\n");
    }

    // Start controller
    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) | XHCI_CMD_RS | XHCI_CMD_INTE | XHCI_CMD_HSEE);
    kprintf("[XHCI] Running. Scanning %d ports...\n", xhci.max_ports);

    // Init HID gamepad layer
    hid_gamepad_init();

    // Enumerate connected ports
    for (int i = 1; i <= xhci.max_ports; i++)
        xhci_handle_port((uint8_t)i);
}
