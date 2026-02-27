// AMD RDNA2 GPU Driver — Phase C: Detection, BAR Mapping, VRAM Size
//
// Follows Linux AMDGPU driver conventions:
//   - Registers are DWORD-indexed (multiply by 4 for byte offset from BAR0)
//   - RREG32(reg) / WREG32(reg, val) for MMIO access
//   - SDMA registers use chip-specific IP base offset
//
// References:
//   Linux: drivers/gpu/drm/amd/amdgpu/
//          drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_offset.h
//          drivers/gpu/drm/amd/include/asic_reg/sdma0/sdma0_5_2_0_offset.h

#include <stdint.h>
#include <stdbool.h>
#include "drivers/gpu/amdgpu.h"
#include "drivers/gpu/amdgpu_regs.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "console.h"
#include "string.h"
#include "timer.h"
#include "heap.h"

// ── Global driver state ───────────────────────────────────────────────────────
static struct {
    amdgpu_info_t info;

    // HHDM-mapped register pointers
    volatile uint8_t *mmio_regs;   // BAR0 mapped
    volatile uint8_t *vram_cpu;    // BAR2 mapped (VRAM aperture)
    volatile uint8_t *doorbells;   // BAR4 mapped

    // VRAM bump allocator
    uint64_t vram_alloc_offset;    // next free byte in VRAM

} gpu;

// ── MMIO access ───────────────────────────────────────────────────────────────
#define RREG32(reg) \
    (*(volatile uint32_t *)((uint8_t *)gpu.mmio_regs + ((uint32_t)(reg) << 2)))
#define WREG32(reg, val) \
    (*(volatile uint32_t *)((uint8_t *)gpu.mmio_regs + ((uint32_t)(reg) << 2)) = (uint32_t)(val))

// SDMA0 access (adds chip-specific base)
#define RREG32_SDMA(reg) RREG32(gpu.info.sdma0_base + (reg))
#define WREG32_SDMA(reg, val) WREG32(gpu.info.sdma0_base + (reg), val)

// ── PCI device ID table ───────────────────────────────────────────────────────
typedef struct {
    uint16_t      device_id;
    amdgpu_chip_t chip;
    const char   *name;
} amdgpu_pci_id_t;

static const amdgpu_pci_id_t amdgpu_id_table[] = {
    // RDNA1 (Navi10)
    { 0x731F, CHIP_NAVI10, "Navi10 [RX 5700 XT]" },
    { 0x7310, CHIP_NAVI10, "Navi10 [RX 5700]" },
    { 0x7312, CHIP_NAVI10, "Navi10 [RX 5700 XT 50th]" },

    // RDNA1 (Navi14)
    { 0x7340, CHIP_NAVI14, "Navi14 [RX 5500 XT]" },
    { 0x7341, CHIP_NAVI14, "Navi14 [RX 5500]" },
    { 0x7347, CHIP_NAVI14, "Navi14 [RX 5500M]" },

    // RDNA2 — Navi21 (RX 6800 / 6900 series)
    { 0x73A0, CHIP_NAVI21, "Navi21 [RX 6900 XT]" },
    { 0x73A1, CHIP_NAVI21, "Navi21 [RX 6900 XT]" },
    { 0x73A2, CHIP_NAVI21, "Navi21 [RX 6800 XT]" },
    { 0x73A3, CHIP_NAVI21, "Navi21 [RX 6800]" },
    { 0x73A5, CHIP_NAVI21, "Navi21 [RX 6950 XT]" },
    { 0x73AB, CHIP_NAVI21, "Navi21 [RX 6900 XTX]" },
    { 0x73AE, CHIP_NAVI21, "Navi21 [RX 6900 XT OEM]" },
    { 0x73AF, CHIP_NAVI21, "Navi21 [RX 6900 XT]" },
    { 0x73BF, CHIP_NAVI21, "Navi21 [RX 6900 XT Liquid]" },

    // RDNA2 — Navi22 (RX 6700 series)
    { 0x73C3, CHIP_NAVI22, "Navi22 [RX 6750 XT]" },
    { 0x73DA, CHIP_NAVI22, "Navi22 [RX 6700]" },
    { 0x73DB, CHIP_NAVI22, "Navi22 [RX 6700]" },
    { 0x73DC, CHIP_NAVI22, "Navi22 [RX 6700]" },
    { 0x73DD, CHIP_NAVI22, "Navi22 [RX 6700]" },
    { 0x73DE, CHIP_NAVI22, "Navi22 [RX 6700]" },
    { 0x73DF, CHIP_NAVI22, "Navi22 [RX 6700 XT]" },
    { 0x73E0, CHIP_NAVI22, "Navi22 [RX 6700M]" },
    { 0x73E1, CHIP_NAVI22, "Navi22 [RX 6700]" },

    // RDNA2 — Navi23 (RX 6600 series)
    { 0x73E3, CHIP_NAVI23, "Navi23 [RX 6650 XT]" },
    { 0x73EF, CHIP_NAVI23, "Navi23 [RX 6600 XT]" },
    { 0x73F0, CHIP_NAVI23, "Navi23 [RX 6600]" },
    { 0x73FA, CHIP_NAVI23, "Navi23 [RX 6600M]" },
    { 0x73FB, CHIP_NAVI23, "Navi23 [RX 6600]" },
    { 0x73FF, CHIP_NAVI23, "Navi23 [RX 6600]" },

    // RDNA2 — Navi24 (RX 6400 / 6500 series)
    { 0x7420, CHIP_NAVI24, "Navi24 [RX 6500 XT]" },
    { 0x7421, CHIP_NAVI24, "Navi24 [RX 6400]" },
    { 0x7422, CHIP_NAVI24, "Navi24 [RX 6500 XT]" },
    { 0x7423, CHIP_NAVI24, "Navi24 [RX 6300M]" },
    { 0x7424, CHIP_NAVI24, "Navi24 [RX 6400]" },
    { 0x7425, CHIP_NAVI24, "Navi24 [RX 6500M]" },
    { 0x743F, CHIP_NAVI24, "Navi24 [RX 6400]" },

    { 0, CHIP_UNKNOWN, NULL } // sentinel
};

// SDMA0 IP base DWORD offset for each chip family
static uint32_t sdma0_base_for_chip(amdgpu_chip_t chip) {
    switch (chip) {
        case CHIP_NAVI21:
        case CHIP_NAVI22:
        case CHIP_NAVI23:
        case CHIP_NAVI24: return 0x4980u; // RDNA2 SDMA v5.2
        case CHIP_NAVI10:
        case CHIP_NAVI14: return 0x4960u; // RDNA1 SDMA v5.0
        default:          return 0x4980u;
    }
}

// ── Chip identification ───────────────────────────────────────────────────────
static bool amdgpu_identify_chip(uint16_t device_id) {
    for (int i = 0; amdgpu_id_table[i].name; i++) {
        if (amdgpu_id_table[i].device_id == device_id) {
            gpu.info.device_id = device_id;
            gpu.info.chip      = amdgpu_id_table[i].chip;
            strncpy(gpu.info.name, amdgpu_id_table[i].name, sizeof(gpu.info.name) - 1);
            gpu.info.sdma0_base = sdma0_base_for_chip(gpu.info.chip);
            return true;
        }
    }
    return false;
}

// ── Scan all PCI devices for AMD GPU ─────────────────────────────────────────
// pci_find_device_by_class finds the first, but we want AMD VID specifically.
static pci_device_t *amdgpu_find_pci_device(void) {
    // Try class-based search first (display controller 03:00 or 03:02)
    pci_device_t *dev = pci_find_device_by_class(0x03, 0x00);
    if (dev && dev->vendor_id == 0x1002) return dev;

    dev = pci_find_device_by_class(0x03, 0x02);
    if (dev && dev->vendor_id == 0x1002) return dev;

    // Fallback: brute-force scan via known AMD VID
    // (pci_find_device handles this if implemented)
    extern pci_device_t *pci_find_device(uint16_t vendor, uint16_t device);
    // Try a few common RDNA2 device IDs
    static const uint16_t common_ids[] = {
        0x73A2, 0x73BF, 0x73DF, 0x73EF, 0x73FF, 0x7420, 0x73A3, 0
    };
    for (int i = 0; common_ids[i]; i++) {
        dev = pci_find_device(0x1002, common_ids[i]);
        if (dev) return dev;
    }
    return NULL;
}

// ── GPU soft reset ────────────────────────────────────────────────────────────
static void amdgpu_soft_reset(void) {
    // 1. Halt command processors
    WREG32(mmCP_ME_CNTL, mmCP_ME_CNTL__ME_HALT_MASK |
                         mmCP_ME_CNTL__PFP_HALT_MASK |
                         mmCP_ME_CNTL__MEC_HALT_MASK);

    // 2. Assert soft reset on CP + RLC
    uint32_t rst = GRBM_SOFT_RESET__SOFT_RESET_CP_MASK |
                   GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK;
    WREG32(mmGRBM_SOFT_RESET, rst);
    (void)RREG32(mmGRBM_SOFT_RESET); // flush

    // 3. Hold for a moment
    for (volatile int d = 0; d < 50000; d++);

    // 4. Release reset
    WREG32(mmGRBM_SOFT_RESET, 0);
    (void)RREG32(mmGRBM_SOFT_RESET); // flush

    // 5. Resume command processors
    WREG32(mmCP_ME_CNTL, 0);

    timer_sleep(1);
}

// ── VRAM size detection ───────────────────────────────────────────────────────
static uint64_t amdgpu_detect_vram(uint32_t bar2_size_bytes) {
    // Strategy 1: try reading FB_SIZE register from MC_HUB
    // This may return 0 if SMU firmware hasn't run yet.
    // Register access is safe — it just might read 0.
    uint32_t fb_size_reg = RREG32(mmMC_VM_FB_SIZE_D3);
    if (fb_size_reg > 0 && fb_size_reg < 0x10000) {
        // Value is typically in MB
        uint64_t vram_mb = (uint64_t)fb_size_reg;
        if (vram_mb >= 1 && vram_mb <= 65536) {
            return vram_mb * 1024 * 1024;
        }
    }

    // Strategy 2: use BAR2 size (VRAM aperture, at minimum what we can access)
    // On systems without ReBAR, this is typically 256MB.
    // On ReBAR-enabled systems, this is the full VRAM.
    if (bar2_size_bytes >= 128 * 1024 * 1024) {
        return (uint64_t)bar2_size_bytes;
    }

    // Fallback: assume minimum RDNA2 VRAM
    return 256ULL * 1024 * 1024;
}

// ── Map a physical range into HHDM ───────────────────────────────────────────
// AMD GPU BARs are typically not in the HHDM range; we need to map them.
// Use vmm_map_page for each 4KB page with WC (Write-Combining) flags.
// FLAG: Present=1, RW=1, PCD=1 (Cache Disable), PWT=1 (Write-Through)
// For MMIO: PCD=1 is important. For VRAM: WC (PAT=1, PCD=0, PWT=1) is better.
#define VMM_FLAG_PRESENT   (1ULL << 0)
#define VMM_FLAG_RW        (1ULL << 1)
#define VMM_FLAG_PWT       (1ULL << 3)  // Write-Through
#define VMM_FLAG_PCD       (1ULL << 4)  // Cache Disable (for MMIO)
#define VMM_FLAG_NX        (1ULL << 63) // No Execute

// Maps size bytes of physical memory at phys into a virtual address.
// Returns virtual address or 0 on failure.
static uintptr_t amdgpu_iomap(uint64_t phys, uint64_t size, bool wc) {
    // Align to page boundary
    uint64_t phys_aligned = phys & ~0xFFFULL;
    uint64_t offset       = phys - phys_aligned;
    uint64_t pages        = (size + offset + 0xFFF) >> 12;

    // Use HHDM for now (works if phys < HHDM limit, which is true for most systems).
    // For BARs above 4GB on high-memory systems, we'd need to allocate VA space.
    // In practice, most GPU BARs are below 4GB or in the lower HHDM.
    uintptr_t virt = phys_aligned + vmm_get_hhdm_offset();

    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_RW | VMM_FLAG_NX;
    if (!wc) {
        flags |= VMM_FLAG_PCD; // Strong Uncached for registers
    } else {
        flags |= VMM_FLAG_PWT; // Write-Combining for VRAM (approximate)
    }

    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page(virt + i * 4096, phys_aligned + i * 4096, flags);
    }

    return virt + (uintptr_t)offset;
}

// ── Public: VRAM bump allocator ───────────────────────────────────────────────
int gpu_bo_alloc(gpu_bo_t *bo, size_t size) {
    if (!gpu.info.present || !bo) return -1;

    // Align to 4KB
    size = (size + 0xFFF) & ~0xFFFULL;

    if (gpu.vram_alloc_offset + size > gpu.info.vram_aperture_bytes) {
        kprintf("[AMDGPU] VRAM OOM: need %zu, have %llu free\n",
                size, gpu.info.vram_aperture_bytes - gpu.vram_alloc_offset);
        return -1;
    }

    bo->gpu_offset = gpu.vram_alloc_offset;
    bo->cpu_addr   = (void *)(gpu.vram_cpu + gpu.vram_alloc_offset);
    bo->cpu_phys   = gpu.info.vram_base_phys + gpu.vram_alloc_offset;
    bo->size       = size;
    bo->valid      = true;

    gpu.vram_alloc_offset += size;
    return 0;
}

void gpu_bo_free(gpu_bo_t *bo) {
    // Bump allocator: no-op. Full allocator is Phase C+.
    if (bo) bo->valid = false;
}

// ── SDMA stubs (implemented in Phase C+) ─────────────────────────────────────
int __attribute__((weak)) amdgpu_sdma_init(void)                          { return -1; }
int __attribute__((weak)) amdgpu_sdma_fill(gpu_bo_t *d, uint64_t o,
                                            uint32_t v, size_t n)         { (void)d;(void)o;(void)v;(void)n; return -1; }
int __attribute__((weak)) amdgpu_sdma_copy(gpu_bo_t *d, uint64_t o,
                                            uint64_t s, size_t n)         { (void)d;(void)o;(void)s;(void)n; return -1; }

// ── Main init ─────────────────────────────────────────────────────────────────
void amdgpu_init(void) {
    memset(&gpu, 0, sizeof(gpu));

    // 1. Find AMD GPU
    pci_device_t *dev = amdgpu_find_pci_device();
    if (!dev) {
        kprintf("[AMDGPU] No AMD GPU found.\n");
        return;
    }

    gpu.info.vendor_id = dev->vendor_id;
    uint16_t did = dev->device_id;

    if (!amdgpu_identify_chip(did)) {
        // Unknown AMD GPU — still try to initialize with generic name
        gpu.info.device_id = did;
        gpu.info.chip      = CHIP_UNKNOWN;
        gpu.info.sdma0_base = 0x4980u;
        // Build a generic name without snprintf
        static const char pfx[] = "AMD GPU [????]";
        memcpy(gpu.info.name, pfx, sizeof(pfx));
        // Patch in the hex device ID
        const char *hex = "0123456789ABCDEF";
        gpu.info.name[9]  = hex[(did >> 12) & 0xF];
        gpu.info.name[10] = hex[(did >>  8) & 0xF];
        gpu.info.name[11] = hex[(did >>  4) & 0xF];
        gpu.info.name[12] = hex[ did        & 0xF];
        kprintf("[AMDGPU] Unknown device %04x:%04x — proceeding anyway.\n",
                dev->vendor_id, did);
    }

    kprintf("[AMDGPU] Found: %s at %02x:%02x.%d\n",
            gpu.info.name, dev->bus, dev->slot, dev->func);

    // 2. Enable PCI bus mastering and memory access
    pci_enable_bus_master(dev);
    pci_enable_memory(dev);

    // 3. Map BAR0 (MMIO register space)
    uint32_t bar0_size = 0;
    uint64_t bar0_phys = pci_get_bar_address(dev, 0, &bar0_size);
    if (!bar0_phys) {
        kprintf("[AMDGPU] BAR0 not mapped!\n");
        return;
    }
    if (bar0_size < 4096) bar0_size = 256 * 1024; // assume 256KB minimum

    gpu.info.reg_base_phys = bar0_phys;
    gpu.info.reg_size      = bar0_size;
    gpu.mmio_regs = (volatile uint8_t *)amdgpu_iomap(bar0_phys, bar0_size, false);

    kprintf("[AMDGPU] BAR0 regs:     0x%016llx (%u KB)\n",
            bar0_phys, bar0_size / 1024);

    // 4. Map BAR2 (VRAM aperture)
    uint32_t bar2_size = 0;
    uint64_t bar2_phys = pci_get_bar_address(dev, 2, &bar2_size);
    if (bar2_phys && bar2_size >= 1024 * 1024) {
        gpu.info.vram_base_phys      = bar2_phys;
        gpu.info.vram_aperture_bytes = bar2_size;
        gpu.vram_cpu = (volatile uint8_t *)amdgpu_iomap(bar2_phys, bar2_size, true);
        kprintf("[AMDGPU] BAR2 VRAM:     0x%016llx (%u MB aperture)\n",
                bar2_phys, bar2_size / (1024 * 1024));
    } else {
        kprintf("[AMDGPU] BAR2 VRAM not accessible (size=%u).\n", bar2_size);
    }

    // 5. Map BAR4 (doorbell space)
    uint32_t bar4_size = 0;
    uint64_t bar4_phys = pci_get_bar_address(dev, 4, &bar4_size);
    if (bar4_phys && bar4_size > 0) {
        gpu.info.db_base_phys = bar4_phys;
        gpu.info.db_size      = bar4_size;
        gpu.doorbells = (volatile uint8_t *)amdgpu_iomap(bar4_phys, bar4_size, false);
        kprintf("[AMDGPU] BAR4 doorbells: 0x%016llx (%u KB)\n",
                bar4_phys, bar4_size / 1024);
    }

    // 6. Read GRBM_STATUS before reset (sanity check)
    uint32_t pre_status = RREG32(mmGRBM_STATUS);
    kprintf("[AMDGPU] GRBM_STATUS pre-reset:  0x%08x%s\n",
            pre_status,
            (pre_status & GRBM_STATUS__GUI_ACTIVE_MASK) ? " (active)" : " (idle)");

    // 7. Soft reset (safe — does not require firmware)
    amdgpu_soft_reset();

    // 8. Read GRBM_STATUS after reset
    uint32_t post_status = RREG32(mmGRBM_STATUS);
    kprintf("[AMDGPU] GRBM_STATUS post-reset: 0x%08x%s\n",
            post_status,
            (post_status & GRBM_STATUS__GUI_ACTIVE_MASK) ? " (active)" : " (idle)");

    // 9. Detect VRAM size
    gpu.info.vram_total_bytes = amdgpu_detect_vram((uint32_t)gpu.info.vram_aperture_bytes);

    // Reserve first 4MB of VRAM for kernel GPU use (command buffers, etc.)
    gpu.vram_alloc_offset = 4ULL * 1024 * 1024;

    // 10. Mark as present and report
    gpu.info.present = true;

    uint64_t vram_mb = gpu.info.vram_total_bytes / (1024 * 1024);
    kprintf("[AMDGPU] VRAM: %llu MB  SDMA0 base: 0x%04x\n",
            vram_mb, gpu.info.sdma0_base);
    kprintf("[AMDGPU] GPU ready. Next: amdgpu_sdma_init()\n");
}

amdgpu_info_t *amdgpu_get_info(void) {
    return gpu.info.present ? &gpu.info : NULL;
}
