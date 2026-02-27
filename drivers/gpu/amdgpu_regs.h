#ifndef AMDGPU_REGS_H
#define AMDGPU_REGS_H

// AMD RDNA2 (GFX10.3) Register Definitions
// All offsets are in DWORDs (×4 for byte offset from BAR0).
// Source: Linux kernel drivers/gpu/drm/amd/include/asic_reg/
//         gc/gc_10_3_0_offset.h, sdma0/sdma0_5_2_0_offset.h

#include <stdint.h>

// ── GRBM (Graphics Register Bus Management) ───────────────────────────────────
#define mmGRBM_STATUS            0x8010  // byte 0x20040
#define mmGRBM_STATUS2           0x8011  // byte 0x20044
#define mmGRBM_SOFT_RESET        0x8020  // byte 0x20080
#define mmGRBM_GFX_INDEX         0x900e  // byte 0x24038

// GRBM_STATUS bits
#define GRBM_STATUS__CMDFIFO_AVAIL_MASK         0x0000001Fu
#define GRBM_STATUS__SRBM_RQ_PENDING_MASK       (1u << 5)
#define GRBM_STATUS__CP_RQ_PENDING_MASK         (1u << 6)
#define GRBM_STATUS__CF_RQ_PENDING_MASK         (1u << 7)
#define GRBM_STATUS__PF_RQ_PENDING_MASK         (1u << 8)
#define GRBM_STATUS__GDS_DMA_RQ_PENDING_MASK    (1u << 9)
#define GRBM_STATUS__ME_BUSY_MASK               (1u << 26)
#define GRBM_STATUS__PFP_BUSY_MASK              (1u << 27)
#define GRBM_STATUS__CP_COHERENCY_BUSY_MASK     (1u << 28)
#define GRBM_STATUS__CP_STAT_MASK               (1u << 29)
#define GRBM_STATUS__GUI_ACTIVE_MASK            (1u << 31)

// GRBM_SOFT_RESET bits
#define GRBM_SOFT_RESET__SOFT_RESET_CP_MASK     (1u << 0)
#define GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK    (1u << 2)

// ── Command Processor (CP) ───────────────────────────────────────────────────
#define mmCP_ME_CNTL             0x81d7  // ME/PFP/MEC reset
#define mmCP_ME_CNTL__ME_HALT_MASK   (1u << 28)
#define mmCP_ME_CNTL__PFP_HALT_MASK  (1u << 26)
#define mmCP_ME_CNTL__MEC_HALT_MASK  (1u << 25)

#define mmCP_RB0_BASE            0x80c3  // GFX ring buffer 0 base (lo)
#define mmCP_RB0_BASE_HI         0x80c4  // GFX ring buffer 0 base (hi)
#define mmCP_RB0_CNTL            0x80c1  // GFX ring buffer 0 control
#define mmCP_RB_RPTR_ADDR        0x80c9  // Ring rptr writeback addr (lo)
#define mmCP_RB_RPTR_ADDR_HI     0x80ca  // Ring rptr writeback addr (hi)
#define mmCP_RB_WPTR             0x80c8  // Ring write pointer

// ── RLC (Run List Controller) ─────────────────────────────────────────────────
#define mmRLC_CNTL               0x4ca2
#define mmRLC_CNTL__RLC_ENABLE_F32_MASK  (1u << 0)

// ── HDP (Host Data Path) ──────────────────────────────────────────────────────
#define mmHDP_HOST_PATH_CNTL     0x0B00  // byte 0x2C00
#define mmHDP_NONSURFACE_BASE    0x0B02
#define mmHDP_NONSURFACE_INFO    0x0B03
#define mmHDP_NONSURFACE_SIZE    0x0B04

// ── Memory Controller / VM ────────────────────────────────────────────────────
// Note: FB_SIZE may not be readable without SMU firmware init.
// Safer to use BAR2 size as VRAM aperture size.
#define mmMC_VM_FB_SIZE_D3       0x0803  // Framebuffer size (MC_HUB)
#define mmMC_VM_FB_OFFSET        0x0800
#define mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB  0x080C
#define mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB  0x080D

// ── SDMA0 register definitions ────────────────────────────────────────────────
// SDMA0 IP base DWORD offset from BAR0 (chip-family-specific):
//   Navi21/22/23 (RDNA2): 0x4980
//   Navi10/14    (RDNA1): 0x4960
// These are added to per-register relative offsets below.

// Relative DWORD offsets within SDMA0 IP block (from sdma0_5_2_0_offset.h)
#define SDMA0_REG_GFX_RB_CNTL          0x0060
#define SDMA0_REG_GFX_RB_BASE          0x0061
#define SDMA0_REG_GFX_RB_BASE_HI       0x0062
#define SDMA0_REG_GFX_RB_RPTR          0x0063
#define SDMA0_REG_GFX_RB_RPTR_HI       0x0064
#define SDMA0_REG_GFX_RB_WPTR          0x0065
#define SDMA0_REG_GFX_RB_WPTR_HI       0x0066
#define SDMA0_REG_GFX_RB_WPTR_POLL_LO  0x0067
#define SDMA0_REG_GFX_RB_WPTR_POLL_HI  0x0068
#define SDMA0_REG_GFX_RB_RPTR_ADDR_HI  0x0069
#define SDMA0_REG_GFX_RB_RPTR_ADDR_LO  0x006A
#define SDMA0_REG_GFX_DOORBELL         0x006B
#define SDMA0_REG_GFX_DOORBELL_OFFSET  0x006D
#define SDMA0_REG_GFX_WATERMARK        0x006E
#define SDMA0_REG_CNTL                 0x00D6
#define SDMA0_REG_F32_CNTL             0x00D8
#define SDMA0_REG_STATUS               0x00F4

// SDMA ring buffer control bits
#define SDMA_GFX_RB_CNTL__RB_ENABLE_MASK        (1u << 0)
#define SDMA_GFX_RB_CNTL__RB_SIZE_SHIFT         1u
#define SDMA_GFX_RB_CNTL__RB_SWAP_ENABLE_MASK   (1u << 9)
#define SDMA_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE (1u << 12)
#define SDMA_GFX_RB_CNTL__RPTR_WRITEBACK_TIMER  (4u << 16)

// ── SDMA Packet opcodes (v5.x, RDNA2) ────────────────────────────────────────
#define SDMA_OP_NOP            0
#define SDMA_OP_COPY           1
#define SDMA_OP_WRITE          2
#define SDMA_OP_INDIRECT       4
#define SDMA_OP_FENCE          5
#define SDMA_OP_TRAP           6
#define SDMA_OP_POLL_REGMEM    8
#define SDMA_OP_CONST_FILL     11
#define SDMA_OP_TIMESTAMP      13

// SDMA_OP_COPY sub-operations
#define SDMA_COPY_SUB_OP_LINEAR  0

// Packet header DW0: op[7:0] | sub_op[15:8] | extra[31:16]
#define SDMA_PKT_HDR(op, sub_op)  ((uint32_t)(op) | ((uint32_t)(sub_op) << 8))

// ── SDMA packet structures ─────────────────────────────────────────────────────

// Copy Linear (7 DWORDs)
typedef struct {
    uint32_t dw0;         // SDMA_PKT_HDR(SDMA_OP_COPY, 0) | count_hi<<20
    uint32_t count;       // byte count (0-based)
    uint32_t rsvd;        // reserved
    uint32_t src_addr_lo;
    uint32_t src_addr_hi;
    uint32_t dst_addr_lo;
    uint32_t dst_addr_hi;
} __attribute__((packed)) sdma_pkt_copy_linear_t;

// Constant Fill (6 DWORDs)
typedef struct {
    uint32_t dw0;         // SDMA_PKT_HDR(SDMA_OP_CONST_FILL, 0)
    uint32_t dst_addr_lo;
    uint32_t dst_addr_hi;
    uint32_t data;        // fill value
    uint32_t count;       // byte count
    uint32_t dw5;         // fillsize[1:0]: 0=byte, 1=word, 2=dword
} __attribute__((packed)) sdma_pkt_const_fill_t;

// Fence (4 DWORDs) — write value to addr when previous commands complete
typedef struct {
    uint32_t dw0;         // SDMA_PKT_HDR(SDMA_OP_FENCE, 0)
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t data;
} __attribute__((packed)) sdma_pkt_fence_t;

// NOP (1 DWORD)
typedef struct {
    uint32_t dw0;         // SDMA_PKT_HDR(SDMA_OP_NOP, 0)
} __attribute__((packed)) sdma_pkt_nop_t;

// ── GPU MMIO access helpers ───────────────────────────────────────────────────
// These are defined as macros in amdgpu.c using the `gpu` global.
// Declared here for documentation purposes.
//
//   RREG32(reg)       -- read  DWORD register
//   WREG32(reg, val)  -- write DWORD register
//   RREG32_SDMA(reg)  -- read  SDMA0 DWORD register (adds SDMA0 base)
//   WREG32_SDMA(reg, val) -- write SDMA0 DWORD register

#endif // AMDGPU_REGS_H
