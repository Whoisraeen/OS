#include "hda.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "serial.h"
#include "heap.h"
#include "string.h"
#include "timer.h"

// Intel HDA Controller State
typedef struct {
    pci_device_t *pci_dev;
    uint8_t *mmio_base;
    uint32_t *corb;
    uint64_t *rirb;
    uint32_t corb_entries;
    uint32_t rirb_entries;
    uint16_t corb_rp;
    uint16_t rirb_wp;
    
    // Codec Info
    int codec_addr;
    
    // DMA State
    uint8_t *bdl_base;
    uint32_t *dma_buf;
    int stream_id;
} hda_state_t;

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint32_t flags; // Bit 0: IOC
} __attribute__((packed)) bdl_entry_t;

static hda_state_t hda;

static uint32_t hda_read32(uint32_t reg) {
    return *(volatile uint32_t *)(hda.mmio_base + reg);
}

static void hda_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(hda.mmio_base + reg) = val;
}

static uint16_t hda_read16(uint32_t reg) {
    return *(volatile uint16_t *)(hda.mmio_base + reg);
}

static void hda_write16(uint32_t reg, uint16_t val) {
    *(volatile uint16_t *)(hda.mmio_base + reg) = val;
}

/*
static uint8_t hda_read8(uint32_t reg) {
    return *(volatile uint8_t *)(hda.mmio_base + reg);
}

static void hda_write8(uint32_t reg, uint8_t val) {
    *(volatile uint8_t *)(hda.mmio_base + reg) = val;
}
*/

static void hda_reset_controller(void) {
    uint32_t gctl = hda_read32(HDA_GCTL);
    hda_write32(HDA_GCTL, gctl & ~1); // Clear CRST
    
    // Wait for reset
    int timeout = 1000;
    while ((hda_read32(HDA_GCTL) & 1) && --timeout) timer_sleep(1);
    
    hda_write32(HDA_GCTL, gctl | 1); // Set CRST
    
    timeout = 1000;
    while (!(hda_read32(HDA_GCTL) & 1) && --timeout) timer_sleep(1);
    
    // Wait for codecs to wake up
    timer_sleep(10);
}

// CORB/RIRB Setup would go here... simplified for now
// We will use Immediate Command Interface (ICW/ICR) for simplicity in this initial driver

static uint32_t hda_send_verb(int codec, int node, uint32_t verb, uint32_t payload) {
    uint32_t cmd = (codec << 28) | (node << 20) | (verb << 8) | payload;
    
    // Wait for ready
    while (hda_read16(HDA_ICIS) & 1);
    
    hda_write32(HDA_ICOI, cmd);
    hda_write16(HDA_ICIS, 1 | 2); // Set BUSY and IRV
    
    // Wait for done
    while (hda_read16(HDA_ICIS) & 1);
    
    if (hda_read16(HDA_ICIS) & 2) { // Valid result
        return hda_read32(HDA_ICII);
    }
    
    return 0xFFFFFFFF;
}

static void hda_setup_output_stream(void) {
    // 1. Find an Output Stream (ISS=Input Streams, GCTL[11:8])
    // GCAP: [15:12] = OSS (Output Streams), [11:8] = ISS (Input Streams), [7:4] = BSS (Bi-Dir)
    uint16_t gcap = hda_read16(HDA_GCAP);
    int iss = (gcap >> 8) & 0x0F;
    int oss = (gcap >> 12) & 0x0F;
    
    kprintf("[HDA] GCAP=%04x (ISS=%d, OSS=%d)\n", gcap, iss, oss);
    
    if (oss == 0) {
        kprintf("[HDA] No Output Streams supported!\n");
        return;
    }
    
    // Use first Output Stream
    // Offset = 0x80 + (NUM_ISS * 0x20) + (STREAM_INDEX * 0x20)
    int stream_idx = 0;
    int reg_offset = 0x80 + (iss * 0x20) + (stream_idx * 0x20);
    hda.stream_id = 1; // Arbitrary stream tag (must be non-zero)
    
    kprintf("[HDA] Configuring Output Stream %d at offset 0x%x\n", stream_idx, reg_offset);
    
    // 2. Reset Stream
    hda_write32(reg_offset + HDA_SD_CTL, 1); // Set SRST
    // Wait for reset
    int timeout = 1000;
    while (!(hda_read32(reg_offset + HDA_SD_CTL) & 1) && --timeout);
    
    hda_write32(reg_offset + HDA_SD_CTL, 0); // Clear SRST
    timeout = 1000;
    while ((hda_read32(reg_offset + HDA_SD_CTL) & 1) && --timeout);
    
    // 3. Setup BDL (Buffer Descriptor List)
    // Allocate BDL (needs alignment?)
    hda.bdl_base = kmalloc(sizeof(bdl_entry_t) * 2); // 2 entries
    memset(hda.bdl_base, 0, sizeof(bdl_entry_t) * 2);
    
    // Allocate DMA Buffer (4KB)
    hda.dma_buf = kmalloc(4096);
    memset(hda.dma_buf, 0, 4096);
    
    // Setup Entry 0
    bdl_entry_t *bdl = (bdl_entry_t *)hda.bdl_base;
    uint64_t phys_addr = (uint64_t)hda.dma_buf; // TODO: virt_to_phys if paging enabled! 
    // Assuming identity mapping for kernel heap for now or that we need a physical allocator
    // Note: This works only if kmalloc returns physically contiguous memory that is 1:1 mapped.
    
    bdl[0].addr = phys_addr;
    bdl[0].len = 4096;
    bdl[0].flags = 1; // IOC
    
    // Write BDL Address to Stream Registers
    uint64_t bdl_phys = (uint64_t)hda.bdl_base;
    hda_write32(reg_offset + HDA_SD_BDLPL, (uint32_t)bdl_phys);
    hda_write32(reg_offset + HDA_SD_BDLPU, (uint32_t)(bdl_phys >> 32));
    
    // 4. Set Cyclic Buffer Length
    hda_write32(reg_offset + HDA_SD_CBL, 4096);
    
    // 5. Set Last Valid Index
    hda_write16(reg_offset + HDA_SD_LVI, 0); // Only 1 entry (index 0)
    
    // 6. Set Stream Format (48kHz, 16-bit, Stereo = 0x0011)
    // Bit 15=0 (PCM), 14=0, 13:11=000 (48k), 10:8=000, 7=0, 6:4=001 (16b), 3:0=0001 (2ch)
    uint16_t fmt = 0x0011;
    hda_write16(reg_offset + HDA_SD_FMT, fmt);
    
    // 7. Setup Codec (Widget Path)
    // This is complex: Pin Complex -> Audio Output (DAC)
    // Simplified: We assume Node 2 (DAC) and Node 3 (Pin) for QEMU/VMWare defaults
    int dac_node = 0x02;
    int pin_node = 0x03; // Line Out / Speaker
    
    // Unmute DAC and Set Gain
    // Set Amp Gain: 0x3 = Set, 0x0 = Output/Left, 0x0 = Index, Gain=0x7F (Max)
    // Payload: 0b0011_xxxx_xxxx_xxxx (Set Amp Gain)
    // Output(1) | Input(0) -> Bit 15. Left(1)|Right(0) -> Bit 13.
    // 0x3000 | (1<<15) | (1<<13) | 0x7F
    
    // Set Stream ID on DAC
    hda_send_verb(hda.codec_addr, dac_node, HDA_VERB_SET_STREAM_CH, (hda.stream_id << 4));
    
    // Set Format on DAC
    hda_send_verb(hda.codec_addr, dac_node, HDA_VERB_SET_FMT, fmt);
    
    // Unmute Pin and Enable Out
    // Pin Widget Control: 0x40 (Out Enable) | 0x80 (Headphone)
    hda_send_verb(hda.codec_addr, pin_node, HDA_VERB_SET_PIN_CTL, 0x40 | 0x80);
    
    // Unmute Pin Amp
    // hda_send_verb(hda.codec_addr, pin_node, HDA_VERB_SET_AMP_GAIN, ...);
    
    kprintf("[HDA] Stream configured. Buffer at %p\n", hda.dma_buf);
    
    // 8. Start Stream
    hda_write32(reg_offset + HDA_SD_CTL, 2 | 4); // RUN | IOCE
    
    kprintf("[HDA] Stream Started!\n");
}

void hda_init(void) {
    // Find Intel HDA (Class 04, Subclass 03)
    pci_device_t *dev = pci_find_device_by_class(0x04, 0x03);
    if (!dev) {
        // Try to find by Vendor ID if class lookup fails or is generic
        dev = pci_find_device(0x8086, 0x2668); // ICH6
        if (!dev) return;
    }
    
    hda.pci_dev = dev;
    pci_enable_bus_master(dev);
    
    // Map MMIO (BAR0)
    uint32_t bar0_size;
    uint64_t bar0_phys = pci_get_bar_address(dev, 0, &bar0_size);
    hda.mmio_base = (uint8_t *)(bar0_phys + vmm_get_hhdm_offset());
    
    kprintf("[HDA] Found controller at %02x:%02x.%d, MMIO at 0x%lx\n",
        dev->bus, dev->slot, dev->func, bar0_phys);
        
    hda_reset_controller();
    
    uint16_t statests = hda_read16(HDA_STATESTS);
    kprintf("[HDA] Codec Status: %04x\n", statests);
    
    // Detect Codec
    for (int i = 0; i < 15; i++) {
        if (statests & (1 << i)) {
            hda.codec_addr = i;
            kprintf("[HDA] Codec found at address %d\n", i);
            
            uint32_t vid = hda_send_verb(i, 0, HDA_VERB_GET_PARAM, HDA_PARAM_VENDOR_ID);
            kprintf("[HDA] Codec Vendor ID: %08x\n", vid);
            break;
        }
    }
    
    // Initialize Streams (TODO)
    hda_setup_output_stream();
}
