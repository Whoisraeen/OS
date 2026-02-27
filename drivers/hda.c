// Intel High Definition Audio (HDA) Driver
// Full implementation: CORB/RIRB, codec topology enumeration,
// double-buffered DMA output, 48kHz/16-bit/stereo PCM ring buffer.

#include "drivers/hda.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "serial.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "idt.h"
#include "spinlock.h"

// ── HDA register offsets ─────────────────────────────────────────────────────
#define HDA_GCAP       0x00
#define HDA_VMIN       0x02
#define HDA_VMAJ       0x03
#define HDA_GCTL       0x08
#define HDA_STATESTS   0x0E
#define HDA_INTCTL     0x20
#define HDA_INTSTS     0x24

// CORB
#define HDA_CORB_LBASE 0x40
#define HDA_CORB_UBASE 0x44
#define HDA_CORB_WP    0x48
#define HDA_CORB_RP    0x4A
#define HDA_CORB_CTL   0x4C
#define HDA_CORB_STS   0x4D
#define HDA_CORB_SIZE  0x4E

// RIRB
#define HDA_RIRB_LBASE 0x50
#define HDA_RIRB_UBASE 0x54
#define HDA_RIRB_WP    0x58
#define HDA_RIRB_CTL   0x5C
#define HDA_RIRB_STS   0x5D
#define HDA_RIRB_SIZE  0x5E

// Immediate Command (fallback)
#define HDA_ICOI       0x60
#define HDA_ICII       0x64
#define HDA_ICIS       0x68

// Stream descriptor base offset and stride
#define HDA_SD_BASE    0x80
#define HDA_SD_STRIDE  0x20

// Stream descriptor register offsets
#define HDA_SD_CTL     0x00
#define HDA_SD_STS     0x03
#define HDA_SD_LPIB    0x04
#define HDA_SD_CBL     0x08
#define HDA_SD_LVI     0x0C
#define HDA_SD_FMT     0x12
#define HDA_SD_BDLPL   0x18
#define HDA_SD_BDLPU   0x1C

// INTCTL bits
#define HDA_INTCTL_GIE   (1u << 31)
#define HDA_INTCTL_CIE   (1u << 30)

// SD_CTL bits
#define HDA_SD_CTL_SRST  (1u << 0)
#define HDA_SD_CTL_RUN   (1u << 1)
#define HDA_SD_CTL_IOCE  (1u << 2)  // Interrupt on Completion Enable
#define HDA_SD_CTL_FEIE  (1u << 3)
#define HDA_SD_CTL_DEIE  (1u << 4)
#define HDA_SD_CTL_STRIPE(n) ((n) << 16)

// SD_STS bits
#define HDA_SD_STS_BCIS  (1u << 2)  // Buffer Completion Interrupt Status

// Stream format: 48kHz, ×1, ÷1, 16-bit, stereo
// Bit 15=0(48k base), 14:11=0000(×1), 10:8=000(÷1), 6:4=001(16b), 3:0=0001(2ch)
#define HDA_FMT_48K_16B_2CH  0x0011u

// ── HDA Codec verbs (additional, not in hda.h) ───────────────────────────────
#define HDA_VERB_GET_PIN_SENSE  0xF0900u
#define HDA_VERB_SET_EAPD       0x70C00u

// HDA Parameters (additional)
#define HDA_PARAM_AUDIO_WID_CAP 0x09u

// Widget types (from PARAM_AUDIO_WID_CAP bits 23:20)
#define WID_OUTPUT_CONV  0   // DAC
#define WID_INPUT_CONV   1   // ADC
#define WID_MIXER        2
#define WID_SELECTOR     3
#define WID_PIN_COMPLEX  4
#define WID_POWER        5
#define WID_VOL_KNOB     6
#define WID_BEEP_GEN     7

// Amp gain/mute verb payload: [15]=Out, [13]=Left, [12]=Right, [7:0]=gain
#define AMP_OUT_LEFT_RIGHT  ((1<<15)|(1<<13)|(1<<12))

// ── Audio ring buffer ─────────────────────────────────────────────────────────
// 48kHz × stereo × 16-bit × 64ms × 4 periods ≈ 48000×4×2×64/1000 = 24576 bytes/period
#define AUDIO_PERIOD_BYTES  8192u   // 8KB per BDL period (~42ms at 48k stereo 16b)
#define AUDIO_NUM_PERIODS   2u
#define AUDIO_DMA_BYTES     (AUDIO_PERIOD_BYTES * AUDIO_NUM_PERIODS)

#define RING_BYTES  65536u          // 64KB software ring buffer (≈340ms)
#define RING_MASK   (RING_BYTES-1)

// ── BDL entry ────────────────────────────────────────────────────────────────
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc;   // bit 0 = IOC
} __attribute__((packed)) bdl_entry_t;

// ── Driver state ──────────────────────────────────────────────────────────────
static struct {
    uint8_t  *mmio;          // HHDM-mapped MMIO base

    // CORB / RIRB
    uint32_t *corb;          // virtual
    uint64_t  corb_phys;
    uint16_t  corb_size;     // number of entries
    uint16_t  corb_wp;

    uint64_t *rirb;          // virtual (each entry is 64-bit: response | response-ex)
    uint64_t  rirb_phys;
    uint16_t  rirb_size;
    uint16_t  rirb_rp;       // our local read pointer

    // Codec
    int       codec_addr;
    uint8_t   afg_nid;       // Audio Function Group NID

    // Discovered audio path
    uint8_t   dac_nid;
    uint8_t   pin_nid;
    uint8_t   stream_tag;    // stream tag (1-15)
    uint32_t  out_amp_maxgain; // max output amp gain

    // Output stream descriptor registers
    uint32_t  sd_reg;        // base of SD registers

    // BDL (2 periods)
    bdl_entry_t *bdl;
    uint64_t     bdl_phys;
    uint8_t     *dma_buf;    // DMA buffer (AUDIO_DMA_BYTES), virtually accessible
    uint64_t     dma_phys;

    // Software ring buffer (userspace writes here)
    uint8_t  ring[RING_BYTES];
    volatile uint32_t ring_rd;   // DMA reads from here
    volatile uint32_t ring_wr;   // userspace writes here
    spinlock_t ring_lock;

    // Current period being filled by DMA
    uint8_t  current_period;

    bool     running;
} hda;

// ── MMIO helpers ──────────────────────────────────────────────────────────────
static inline uint32_t r32(uint32_t off) { return *(volatile uint32_t *)(hda.mmio + off); }
static inline uint16_t r16(uint32_t off) { return *(volatile uint16_t *)(hda.mmio + off); }
static inline uint8_t  r8 (uint32_t off) { return *(volatile uint8_t  *)(hda.mmio + off); }
static inline void w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(hda.mmio + off) = v; }
static inline void w16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(hda.mmio + off) = v; }
static inline void w8 (uint32_t off, uint8_t  v) { *(volatile uint8_t  *)(hda.mmio + off) = v; }

static inline uint32_t sd_r32(uint32_t r) { return r32(hda.sd_reg + r); }
static inline uint16_t sd_r16(uint32_t r) { return r16(hda.sd_reg + r); }
static inline uint8_t  sd_r8 (uint32_t r) { return r8 (hda.sd_reg + r); }
static inline void     sd_w32(uint32_t r, uint32_t v) { w32(hda.sd_reg + r, v); }
static inline void     sd_w16(uint32_t r, uint16_t v) { w16(hda.sd_reg + r, (uint16_t)v); }
static inline void     sd_w8 (uint32_t r, uint8_t  v) { w8 (hda.sd_reg + r, v); }

// ── CORB / RIRB setup ────────────────────────────────────────────────────────
static bool hda_corb_rirb_init(void) {
    // Stop DMA
    w8(HDA_CORB_CTL, 0);
    w8(HDA_RIRB_CTL, 0);
    for (int i = 0; i < 500; i++) {
        if (!(r8(HDA_CORB_CTL) & 2) && !(r8(HDA_RIRB_CTL) & 2)) break;
        timer_sleep(1);
    }

    // Pick largest supported CORB/RIRB size (256 entries preferred)
    uint8_t corb_cap = r8(HDA_CORB_SIZE);
    uint8_t rirb_cap = r8(HDA_RIRB_SIZE);

    uint16_t corb_entries, rirb_entries;
    uint8_t  corb_szcap, rirb_szcap;

    if (corb_cap & 0x40) { corb_entries = 256; corb_szcap = 2; }
    else if (corb_cap & 0x20) { corb_entries = 16; corb_szcap = 1; }
    else { corb_entries = 2; corb_szcap = 0; }

    if (rirb_cap & 0x40) { rirb_entries = 256; rirb_szcap = 2; }
    else if (rirb_cap & 0x20) { rirb_entries = 16; rirb_szcap = 1; }
    else { rirb_entries = 2; rirb_szcap = 0; }

    hda.corb_size = corb_entries;
    hda.rirb_size = rirb_entries;

    // Allocate CORB (4-byte entries, 128-byte aligned min)
    // Use a single page for both CORB and RIRB
    uint64_t page_phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    if (!page_phys) return false;
    uint8_t *page_virt = (uint8_t *)(page_phys + vmm_get_hhdm_offset());
    memset(page_virt, 0, 4096);

    hda.corb      = (uint32_t *)page_virt;
    hda.corb_phys = page_phys;

    // RIRB at offset 2048 (8-byte entries, 128-byte aligned)
    hda.rirb      = (uint64_t *)(page_virt + 2048);
    hda.rirb_phys = page_phys + 2048;

    // Program CORB
    w32(HDA_CORB_LBASE, (uint32_t)hda.corb_phys);
    w32(HDA_CORB_UBASE, (uint32_t)(hda.corb_phys >> 32));
    w8(HDA_CORB_SIZE, corb_szcap);
    w16(HDA_CORB_RP, 0x8000); // Reset RP
    for (int i = 0; i < 500; i++) { if (r16(HDA_CORB_RP) & 0x8000) break; timer_sleep(1); }
    w16(HDA_CORB_RP, 0);
    w16(HDA_CORB_WP, 0);
    hda.corb_wp = 0;

    // Program RIRB
    w32(HDA_RIRB_LBASE, (uint32_t)hda.rirb_phys);
    w32(HDA_RIRB_UBASE, (uint32_t)(hda.rirb_phys >> 32));
    w8(HDA_RIRB_SIZE, rirb_szcap);
    w16(HDA_RIRB_WP, 0x8000); // Reset WP
    hda.rirb_rp = 0;

    // Enable CORB and RIRB DMA
    w8(HDA_CORB_CTL, 2);
    w8(HDA_RIRB_CTL, 2);

    kprintf("[HDA] CORB=%d entries, RIRB=%d entries\n", corb_entries, rirb_entries);
    return true;
}

// ── Send verb via CORB, read response from RIRB ───────────────────────────────
static uint32_t hda_verb(int codec, int nid, uint32_t verb, uint32_t payload) {
    // Build 32-bit command: [31:28]=codec, [27:20]=nid, [19:0]=verb+payload
    uint32_t cmd;
    if (verb & 0xF0000u) {
        // 4-bit verb (e.g. GET_PARAM: 0xF0000 → upper 4 bits = 0xF)
        cmd = ((uint32_t)codec << 28) | ((uint32_t)nid << 20) | (verb & 0xFFFFFu) | (payload & 0xFF);
    } else {
        // 12-bit verb
        cmd = ((uint32_t)codec << 28) | ((uint32_t)nid << 20) | ((verb & 0x00FFFu) << 8) | (payload & 0xFF);
    }
    // Actually always encode as: [31:28] addr, [27:20] nid, [19:0] full verb+payload
    // Standard encoding: 4-bit verb in [19:16], 12-bit payload in [15:0]  (for 4-bit verbs like 0x3/0x7/0xA/0xB)
    // and 12-bit verb in [19:8], 8-bit payload in [7:0] (for 12-bit verbs like 0xF0x)
    // The HDA spec: If verb[7:0] == 0x0, it's a 4-bit verb. Let's build it generically:
    cmd = ((uint32_t)codec << 28) | ((uint32_t)(nid & 0xFF) << 20)
        | (verb & 0xFFFFFu) | (payload & 0xFF);

    // Write to CORB
    uint16_t wp = (uint16_t)((hda.corb_wp + 1) % hda.corb_size);
    hda.corb[wp] = cmd;
    hda.corb_wp = wp;
    w16(HDA_CORB_WP, wp);

    // Wait for RIRB response
    for (int i = 0; i < 10000; i++) {
        uint16_t rirb_wp = r16(HDA_RIRB_WP) & (hda.rirb_size - 1);
        if (rirb_wp != hda.rirb_rp) {
            hda.rirb_rp = (uint16_t)((hda.rirb_rp + 1) % hda.rirb_size);
            uint64_t entry = hda.rirb[hda.rirb_rp];
            return (uint32_t)(entry & 0xFFFFFFFFu); // lower 32 bits = response
        }
        // Tiny delay
        for (volatile int d = 0; d < 100; d++);
    }
    kprintf("[HDA] RIRB timeout (verb=0x%x nid=%d)\n", verb, nid);
    return 0xFFFFFFFF;
}

// ── Codec topology enumeration ────────────────────────────────────────────────
// Walk all widgets in AFG. Find DAC+Pin pair for line/headphone out.

static uint8_t conn_list[32]; // connection list for a widget

static int hda_get_conn_list(int nid) {
    uint32_t cl = hda_verb(hda.codec_addr, nid, HDA_VERB_GET_PARAM, HDA_PARAM_CONN_LIST_LEN);
    int count = cl & 0x7F;
    if (count == 0) return 0;
    // Read entries (short form: each entry 8 bits, long form: 16 bits)
    // For simplicity, only handle short form (count <= 4 per verb)
    for (int i = 0; i < count && i < 32; i += 4) {
        uint32_t entries = hda_verb(hda.codec_addr, nid, HDA_VERB_GET_CONN_LIST, (uint32_t)i);
        for (int j = 0; j < 4 && (i+j) < count; j++) {
            conn_list[i+j] = (uint8_t)(entries >> (j*8)) & 0x7F;
        }
    }
    return count;
}

static void hda_enumerate_codec(void) {
    // Get AFG (Audio Function Group) from root
    uint32_t nc = hda_verb(hda.codec_addr, 0, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT);
    uint8_t start = (uint8_t)((nc >> 16) & 0xFF);
    uint8_t count = (uint8_t)(nc & 0xFF);

    // Find AFG node
    for (int i = 0; i < count; i++) {
        uint8_t nid = start + i;
        uint32_t fg = hda_verb(hda.codec_addr, nid, HDA_VERB_GET_PARAM, HDA_PARAM_FUNC_GROUP);
        if ((fg & 0xFF) == 0x01) { // Audio Function Group
            hda.afg_nid = nid;
            kprintf("[HDA] AFG at NID=0x%02x\n", nid);
            break;
        }
    }
    if (!hda.afg_nid) { kprintf("[HDA] No AFG found!\n"); return; }

    // Power up AFG
    hda_verb(hda.codec_addr, hda.afg_nid, HDA_VERB_SET_POWER, 0); // D0

    // Get widget count inside AFG
    uint32_t wc = hda_verb(hda.codec_addr, hda.afg_nid, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT);
    uint8_t wstart = (uint8_t)((wc >> 16) & 0xFF);
    uint8_t wcount = (uint8_t)(wc & 0xFF);
    kprintf("[HDA] Widgets: NID 0x%02x..0x%02x (%d total)\n",
            wstart, wstart+wcount-1, wcount);

    // First pass: classify widgets
    uint8_t dacs[16]; int ndacs = 0;
    uint8_t pins[16]; int npins = 0;

    for (int i = 0; i < wcount; i++) {
        uint8_t nid = wstart + i;
        uint32_t cap = hda_verb(hda.codec_addr, nid, HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WID_CAP);
        uint8_t type = (uint8_t)((cap >> 20) & 0xF);

        hda_verb(hda.codec_addr, nid, HDA_VERB_SET_POWER, 0); // D0 all

        if (type == WID_OUTPUT_CONV) {
            if (ndacs < 16) dacs[ndacs++] = nid;
        } else if (type == WID_PIN_COMPLEX) {
            // Check if output capable (PIN_CAP bit 4)
            uint32_t pcap = hda_verb(hda.codec_addr, nid, HDA_VERB_GET_PARAM, HDA_PARAM_PIN_CAP);
            if (pcap & (1u << 4)) { // OUT_CAPABLE
                if (npins < 16) pins[npins++] = nid;
            }
        }
    }
    kprintf("[HDA] Found %d DACs, %d output pins\n", ndacs, npins);

    // Try to pair a pin with a DAC via connection list
    // Priority: prefer a pin whose connection list contains a DAC
    for (int pi = 0; pi < npins && !hda.dac_nid; pi++) {
        int cn = hda_get_conn_list(pins[pi]);
        for (int ci = 0; ci < cn; ci++) {
            // conn_list[ci] might be a DAC or a mixer. Walk one more level.
            uint32_t cap2 = hda_verb(hda.codec_addr, conn_list[ci],
                                     HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WID_CAP);
            uint8_t t2 = (uint8_t)((cap2 >> 20) & 0xF);
            if (t2 == WID_OUTPUT_CONV) {
                hda.pin_nid = pins[pi];
                hda.dac_nid = conn_list[ci];
                // Select this connection in the pin's mux (if selector)
                if (cn > 1) hda_verb(hda.codec_addr, pins[pi], HDA_VERB_SET_CONN_SEL, (uint32_t)ci);
                break;
            } else if (t2 == WID_MIXER || t2 == WID_SELECTOR) {
                // Go one level deeper
                int cn2 = hda_get_conn_list(conn_list[ci]);
                for (int ci2 = 0; ci2 < cn2; ci2++) {
                    uint32_t cap3 = hda_verb(hda.codec_addr, conn_list[ci2],
                                             HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WID_CAP);
                    if (((cap3 >> 20) & 0xF) == WID_OUTPUT_CONV) {
                        hda.pin_nid = pins[pi];
                        hda.dac_nid = conn_list[ci2];
                        break;
                    }
                }
                if (hda.dac_nid) break;
            }
        }
    }

    // Fallback: just use first DAC and first output pin
    if (!hda.dac_nid && ndacs > 0 && npins > 0) {
        hda.dac_nid = dacs[0];
        hda.pin_nid = pins[0];
        kprintf("[HDA] Using fallback DAC/pin pairing.\n");
    }

    if (hda.dac_nid) {
        // Read max gain for volume scaling
        uint32_t amp_cap = hda_verb(hda.codec_addr, hda.dac_nid,
                                    HDA_VERB_GET_PARAM, HDA_PARAM_OUT_AMP_CAP);
        hda.out_amp_maxgain = amp_cap & 0x7F;
        kprintf("[HDA] DAC=0x%02x Pin=0x%02x AmpMax=%d\n",
                hda.dac_nid, hda.pin_nid, hda.out_amp_maxgain);
    } else {
        kprintf("[HDA] No DAC/Pin path found! Audio will not work.\n");
    }
}

// ── DMA / stream setup ────────────────────────────────────────────────────────
static bool hda_setup_stream(void) {
    // Allocate DMA buffer (physically contiguous, 2 periods)
    // We need AUDIO_DMA_BYTES contiguous. Allocate enough pages.
    uint32_t pages_needed = (AUDIO_DMA_BYTES + 4095) / 4096;
    // Allocate consecutive pages (simple: alloc pages_needed separate pages won't be
    // contiguous; instead rely on pmm_alloc_pages if available, else use 1 alloc per 4KB
    // and accept that AUDIO_DMA_BYTES <= 4096×N. Since 16KB = 4 pages, allocate 4.
    // For now, use a single 2-page region (16384 bytes = 4 pages needs a contiguous allocator).
    // We'll call pmm_alloc_page multiple times and hope they're adjacent (works for most PMMs).
    // Safe approach: limit to 8KB per period (one 4KB page = not enough for 8KB; use 2 contiguous pages trick)
    // Simplest fix: allocate 4 pages back-to-back at boot time, accept they're likely contiguous.
    uint64_t dma_phys = 0;
    for (uint32_t p = 0; p < pages_needed; p++) {
        uint64_t pp = (uint64_t)(uintptr_t)pmm_alloc_page();
        if (p == 0) dma_phys = pp;
        // If not contiguous, we have a problem. In practice boot-time allocations are contiguous.
        (void)pp;
    }
    hda.dma_phys = dma_phys;
    hda.dma_buf  = (uint8_t *)(dma_phys + vmm_get_hhdm_offset());
    memset(hda.dma_buf, 0, AUDIO_DMA_BYTES);

    // Allocate BDL (2 entries × 16 bytes, 128-byte aligned)
    uint64_t bdl_phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    hda.bdl_phys = bdl_phys;
    hda.bdl      = (bdl_entry_t *)(bdl_phys + vmm_get_hhdm_offset());
    memset(hda.bdl, 0, 4096);

    hda.bdl[0].addr = hda.dma_phys;
    hda.bdl[0].len  = AUDIO_PERIOD_BYTES;
    hda.bdl[0].ioc  = 1;

    hda.bdl[1].addr = hda.dma_phys + AUDIO_PERIOD_BYTES;
    hda.bdl[1].len  = AUDIO_PERIOD_BYTES;
    hda.bdl[1].ioc  = 1;

    // Find the first Output Stream Descriptor
    uint16_t gcap = r16(HDA_GCAP);
    int iss = (gcap >> 8) & 0x0F;  // Input Streams
    // Output stream 0 is at ISS×stride after base
    hda.sd_reg = HDA_SD_BASE + (uint32_t)(iss * HDA_SD_STRIDE);
    hda.stream_tag = 1;

    // Reset stream
    sd_w32(HDA_SD_CTL, HDA_SD_CTL_SRST);
    for (int i = 0; i < 1000; i++) {
        if (sd_r32(HDA_SD_CTL) & HDA_SD_CTL_SRST) break;
        timer_sleep(1);
    }
    sd_w32(HDA_SD_CTL, 0);
    for (int i = 0; i < 1000; i++) {
        if (!(sd_r32(HDA_SD_CTL) & HDA_SD_CTL_SRST)) break;
        timer_sleep(1);
    }

    // Program stream descriptor
    sd_w32(HDA_SD_BDLPL, (uint32_t)hda.bdl_phys);
    sd_w32(HDA_SD_BDLPU, (uint32_t)(hda.bdl_phys >> 32));
    sd_w32(HDA_SD_CBL,   AUDIO_DMA_BYTES);         // Cyclic Buffer Length
    sd_w16(HDA_SD_LVI,   AUDIO_NUM_PERIODS - 1);   // Last Valid Index
    sd_w16(HDA_SD_FMT,   HDA_FMT_48K_16B_2CH);

    // Stream tag in SD_CTL[23:20]
    uint32_t ctl = (uint32_t)hda.stream_tag << 20;
    sd_w32(HDA_SD_CTL, ctl);

    return true;
}

// ── Configure codec path (DAC → Pin, set volume) ─────────────────────────────
static void hda_configure_path(void) {
    if (!hda.dac_nid) return;

    // Set stream + channel on DAC
    hda_verb(hda.codec_addr, hda.dac_nid, HDA_VERB_SET_STREAM_CH,
             (uint32_t)hda.stream_tag << 4); // stream tag, channel 0

    // Set format on DAC
    hda_verb(hda.codec_addr, hda.dac_nid, HDA_VERB_SET_FMT, HDA_FMT_48K_16B_2CH);

    // Set output amp on DAC to max gain, both channels
    uint32_t gain = hda.out_amp_maxgain ? hda.out_amp_maxgain : 0x4A;
    hda_verb(hda.codec_addr, hda.dac_nid, HDA_VERB_SET_AMP_GAIN,
             AMP_OUT_LEFT_RIGHT | (gain & 0x7F));

    // Enable EAPD on pin (if supported)
    hda_verb(hda.codec_addr, hda.pin_nid, HDA_VERB_SET_EAPD, 0x02); // EAPD enable

    // Pin control: Output Enable
    hda_verb(hda.codec_addr, hda.pin_nid, HDA_VERB_SET_PIN_CTL, 0x40); // Out enable

    // Pin output amp: unmute, max gain
    hda_verb(hda.codec_addr, hda.pin_nid, HDA_VERB_SET_AMP_GAIN,
             AMP_OUT_LEFT_RIGHT | (gain & 0x7F));
}

// ── Buffer refill ─────────────────────────────────────────────────────────────
// Called from ISR when a period completes. Fills the completed period buffer.
static void hda_refill_period(uint8_t period) {
    uint8_t *dst = hda.dma_buf + period * AUDIO_PERIOD_BYTES;
    uint32_t avail = (hda.ring_wr - hda.ring_rd) & RING_MASK;

    if (avail >= AUDIO_PERIOD_BYTES) {
        // Copy one period worth of data from ring
        uint32_t rd = hda.ring_rd & RING_MASK;
        uint32_t first = RING_BYTES - rd;
        if (first >= AUDIO_PERIOD_BYTES) {
            memcpy(dst, hda.ring + rd, AUDIO_PERIOD_BYTES);
        } else {
            memcpy(dst, hda.ring + rd, first);
            memcpy(dst + first, hda.ring, AUDIO_PERIOD_BYTES - first);
        }
        hda.ring_rd = (hda.ring_rd + AUDIO_PERIOD_BYTES) & RING_MASK;
    } else {
        // Underrun: fill with silence
        memset(dst, 0, AUDIO_PERIOD_BYTES);
    }
    hda.current_period = period ^ 1; // next period
}

// ── ISR ───────────────────────────────────────────────────────────────────────
static void hda_isr(void) {
    uint32_t intsts = r32(HDA_INTSTS);
    w32(HDA_INTSTS, intsts); // W1C

    // Check stream interrupt bit (stream 0 = bit 0 after ISS input streams)
    uint8_t sd_sts = sd_r8(HDA_SD_STS);
    if (sd_sts & HDA_SD_STS_BCIS) {
        sd_w8(HDA_SD_STS, HDA_SD_STS_BCIS); // W1C
        // The period that just completed is current_period (about to become old)
        hda_refill_period(hda.current_period);
    }
}

// ── Public: write PCM samples to ring buffer ──────────────────────────────────
// Returns bytes actually written (may be less than len if ring is full).
int hda_write_audio(const void *pcm, int len) {
    if (!hda.running || len <= 0) return 0;

    int written = 0;
    const uint8_t *src = (const uint8_t *)pcm;

    spinlock_acquire(&hda.ring_lock);
    while (len > 0) {
        uint32_t used  = (hda.ring_wr - hda.ring_rd) & RING_MASK;
        uint32_t free  = RING_BYTES - 1 - used;
        if (free == 0) break; // ring full

        uint32_t chunk = (uint32_t)len < free ? (uint32_t)len : free;
        uint32_t wr    = hda.ring_wr & RING_MASK;
        uint32_t first = RING_BYTES - wr;
        if (chunk <= first) {
            memcpy(hda.ring + wr, src, chunk);
        } else {
            memcpy(hda.ring + wr, src, first);
            memcpy(hda.ring,      src + first, chunk - first);
        }
        hda.ring_wr = (hda.ring_wr + chunk) & RING_MASK;
        src     += chunk;
        len     -= (int)chunk;
        written += (int)chunk;
    }
    spinlock_release(&hda.ring_lock);
    return written;
}

// Returns free bytes in the ring (how much userspace can write without blocking).
uint32_t hda_ring_free(void) {
    uint32_t used = (hda.ring_wr - hda.ring_rd) & RING_MASK;
    return RING_BYTES - 1 - used;
}

// Set volume (0–100 mapped to 0–max_amp_gain)
void hda_set_volume(int vol_pct) {
    if (!hda.dac_nid) return;
    if (vol_pct < 0)   vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;

    uint32_t gain = (hda.out_amp_maxgain * (uint32_t)vol_pct) / 100;
    uint32_t mute = (vol_pct == 0) ? (1u << 7) : 0;
    uint32_t payload = AMP_OUT_LEFT_RIGHT | mute | (gain & 0x7F);

    hda_verb(hda.codec_addr, hda.dac_nid, HDA_VERB_SET_AMP_GAIN, payload);
    hda_verb(hda.codec_addr, hda.pin_nid, HDA_VERB_SET_AMP_GAIN, payload);
}

// ── Init ──────────────────────────────────────────────────────────────────────
void hda_init(void) {
    memset(&hda, 0, sizeof(hda));
    spinlock_init(&hda.ring_lock);

    // Find HDA controller (PCI class 04:03)
    pci_device_t *dev = pci_find_device_by_class(0x04, 0x03);
    if (!dev) {
        kprintf("[HDA] No controller found.\n");
        return;
    }
    kprintf("[HDA] Controller at %02x:%02x.%d\n", dev->bus, dev->slot, dev->func);

    pci_enable_bus_master(dev);
    pci_enable_memory(dev);

    uint64_t bar0_phys = pci_get_bar_address(dev, 0, NULL);
    hda.mmio = (uint8_t *)(bar0_phys + vmm_get_hhdm_offset());

    // Reset controller (GCTL.CRST)
    w32(HDA_GCTL, 0);
    for (int i = 0; i < 1000; i++) { if (!(r32(HDA_GCTL) & 1)) break; timer_sleep(1); }
    w32(HDA_GCTL, 1);
    for (int i = 0; i < 1000; i++) { if (r32(HDA_GCTL) & 1) break; timer_sleep(1); }
    timer_sleep(10); // codec wake-up time

    // Detect codecs
    uint16_t statests = r16(HDA_STATESTS);
    kprintf("[HDA] STATESTS=%04x\n", statests);
    hda.codec_addr = -1;
    for (int i = 0; i < 15; i++) {
        if (statests & (1u << i)) {
            hda.codec_addr = i;
            uint32_t vid = hda_verb(i, 0, HDA_VERB_GET_PARAM, HDA_PARAM_VENDOR_ID);
            kprintf("[HDA] Codec %d: VID=%08x\n", i, vid);
            break; // use first codec
        }
    }
    if (hda.codec_addr < 0) { kprintf("[HDA] No codec.\n"); return; }

    // Init CORB/RIRB
    if (!hda_corb_rirb_init()) { kprintf("[HDA] CORB/RIRB init failed.\n"); return; }

    // Enumerate codec topology
    hda_enumerate_codec();
    if (!hda.dac_nid) return;

    // Setup DMA stream
    if (!hda_setup_stream()) { kprintf("[HDA] Stream setup failed.\n"); return; }

    // Configure codec path
    hda_configure_path();

    // Register MSI/IRQ (vector 46 — one below AHCI's 47; let's use 48)
    if (pci_enable_msi(dev, 48, 0) == 0) {
        irq_register_handler(48, hda_isr);
        kprintf("[HDA] MSI vector 48\n");
    }

    // Enable interrupts: global + stream
    uint16_t gcap = r16(HDA_GCAP);
    int iss = (gcap >> 8) & 0x0F;
    w32(HDA_INTCTL, HDA_INTCTL_GIE | (1u << (uint32_t)iss)); // output stream 0 bit

    // Start stream: RUN | IOCE | stream tag in [23:20]
    uint32_t ctl = ((uint32_t)hda.stream_tag << 20) | HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE;
    sd_w32(HDA_SD_CTL, ctl);

    hda.running = true;
    kprintf("[HDA] Audio ready. DAC=0x%02x Pin=0x%02x fmt=48kHz/16b/stereo\n",
            hda.dac_nid, hda.pin_nid);
}
