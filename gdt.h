#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// GDT Entry Structure (8 bytes)
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

// GDT Pointer Structure (for lgdt)
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// TSS Entry (System Segment in 64-bit - 16 bytes)
struct tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

// Task State Segment (64-bit TSS)
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          // Kernel stack for Ring 0
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;          // Interrupt Stack Table entries
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

// GDT Segment selectors
#define GDT_KERNEL_CODE 0x08  // Entry 1, Ring 0
#define GDT_KERNEL_DATA 0x10  // Entry 2, Ring 0
#define GDT_USER_DATA   0x1B  // Entry 3, Ring 3 (0x18 | 3) - Swapped for SYSCALL
#define GDT_USER_CODE   0x23  // Entry 4, Ring 3 (0x20 | 3) - Swapped for SYSCALL
#define GDT_TSS         0x28  // Entry 5, TSS

// Initialize GDT
void gdt_init(void);

// Setup GDT for a specific CPU
void gdt_setup_cpu(struct gdt_entry *gdt_base, struct tss *tss_base);
void gdt_set_gate_ptr(struct gdt_entry *gdt_base, int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran);

// Set kernel stack for TSS (called before returning to Ring 3)
void tss_set_kernel_stack(uint64_t stack);

// Get TSS struct (for external use)
struct tss *get_tss(void);

#endif
