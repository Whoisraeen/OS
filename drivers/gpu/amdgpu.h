#ifndef AMDGPU_H
#define AMDGPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ── Chip families ─────────────────────────────────────────────────────────────
typedef enum {
    CHIP_NAVI10  = 0,  // RDNA1: RX 5700 XT
    CHIP_NAVI14,       // RDNA1: RX 5500 XT
    CHIP_NAVI21,       // RDNA2: RX 6800 / 6900 XT
    CHIP_NAVI22,       // RDNA2: RX 6700 XT
    CHIP_NAVI23,       // RDNA2: RX 6600 XT
    CHIP_NAVI24,       // RDNA2: RX 6400 / 6500 XT
    CHIP_UNKNOWN,
} amdgpu_chip_t;

// ── GPU information (read-only after init) ────────────────────────────────────
typedef struct {
    bool          present;          // GPU was detected and initialized

    uint16_t      vendor_id;        // 0x1002
    uint16_t      device_id;
    amdgpu_chip_t chip;
    char          name[48];         // e.g. "Navi21 [RX 6900 XT]"

    // Physical BAR addresses
    uint64_t      reg_base_phys;    // BAR0: MMIO register space
    uint32_t      reg_size;         // BAR0 size in bytes

    uint64_t      vram_base_phys;   // BAR2: VRAM aperture
    uint64_t      vram_aperture_bytes; // BAR2 size

    uint64_t      db_base_phys;     // BAR4: Doorbell space
    uint32_t      db_size;

    // VRAM info
    uint64_t      vram_total_bytes; // Actual VRAM (from register or BAR2)

    // SDMA IP base (DWORD offset from BAR0, chip-specific)
    uint32_t      sdma0_base;
} amdgpu_info_t;

// ── Buffer Object (VRAM allocation) ──────────────────────────────────────────
typedef struct {
    uint64_t  gpu_offset;   // byte offset into VRAM
    void     *cpu_addr;     // CPU-accessible pointer (via VRAM aperture)
    uint64_t  cpu_phys;     // physical address (for SDMA as src/dst)
    size_t    size;
    bool      valid;
} gpu_bo_t;

// ── Public API ────────────────────────────────────────────────────────────────

// Initialize: detect AMD GPU, map BARs, reset, report info.
void amdgpu_init(void);

// Returns pointer to GPU info struct, or NULL if no GPU.
amdgpu_info_t *amdgpu_get_info(void);

// VRAM allocator (bump allocator, no free for now)
int  gpu_bo_alloc(gpu_bo_t *bo, size_t size);
void gpu_bo_free(gpu_bo_t *bo);   // stub — currently no-op

// SDMA (Phase C+)
// These are declared here but implemented in amdgpu_sdma.c (future)
int amdgpu_sdma_init(void);
int amdgpu_sdma_fill(gpu_bo_t *dst, uint64_t dst_off, uint32_t value, size_t bytes);
int amdgpu_sdma_copy(gpu_bo_t *dst, uint64_t dst_off,
                     uint64_t src_phys, size_t bytes);

#endif // AMDGPU_H
