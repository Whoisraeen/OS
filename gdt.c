#include "gdt.h"

// GDT: 5 regular entries + 1 TSS entry (TSS takes 2 slots in 64-bit)
struct gdt_entry gdt[7];
struct gdt_ptr gp;
struct tss kernel_tss;

// Implemented in interrupts.S
extern void global_gdt_flush(uint64_t gdt_ptr);
extern void tss_flush(void);

// GDT entries (copied from gdt.c)
// We need to replicate GDT setup for each core
// Make this function non-static so cpu.c can use it, or move it to a shared header?
// Better: Refactor gdt.c to export a setup function
void gdt_setup_cpu(struct gdt_entry *gdt_base, struct tss *tss_base) {
    // 0: Null
    gdt_set_gate_ptr(gdt_base, 0, 0, 0, 0, 0);
    
    // 1: Kernel Code
    gdt_set_gate_ptr(gdt_base, 1, 0, 0xFFFFFFFF, 0x9A, 0xA0);
    
    // 2: Kernel Data
    gdt_set_gate_ptr(gdt_base, 2, 0, 0xFFFFFFFF, 0x92, 0xA0);
    
    // 3: User Data
    gdt_set_gate_ptr(gdt_base, 3, 0, 0xFFFFFFFF, 0xF2, 0xA0);
    
    // 4: User Code
    gdt_set_gate_ptr(gdt_base, 4, 0, 0xFFFFFFFF, 0xFA, 0xA0);
    
    // 5-6: TSS
    uint64_t tss_addr = (uint64_t)tss_base;
    uint32_t tss_limit = sizeof(struct tss) - 1;
    
    struct tss_entry *tss_desc = (struct tss_entry *)&gdt_base[5];
    tss_desc->limit_low    = tss_limit & 0xFFFF;
    tss_desc->base_low     = tss_addr & 0xFFFF;
    tss_desc->base_middle  = (tss_addr >> 16) & 0xFF;
    tss_desc->access       = 0x89;
    tss_desc->granularity  = ((tss_limit >> 16) & 0x0F);
    tss_desc->base_high    = (tss_addr >> 24) & 0xFF;
    tss_desc->base_upper   = (tss_addr >> 32) & 0xFFFFFFFF;
    tss_desc->reserved     = 0;
}

static void gdt_set_gate(int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran) {
    gdt_set_gate_ptr(gdt, num, base, limit, access, gran);
}

// Helper that takes a pointer to the entry
void gdt_set_gate_ptr(struct gdt_entry *gdt_base, int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran) {
    gdt_base[num].base_low    = (base & 0xFFFF);
    gdt_base[num].base_middle = (base >> 16) & 0xFF;
    gdt_base[num].base_high   = (base >> 24) & 0xFF;

    gdt_base[num].limit_low   = (limit & 0xFFFF);
    gdt_base[num].granularity = (limit >> 16) & 0x0F;

    gdt_base[num].granularity |= (gran & 0xF0);
    gdt_base[num].access      = access;
}

static void tss_set_gate(int num, uint64_t base, uint64_t limit) {
    // TSS descriptor is 16 bytes in 64-bit mode
    struct tss_entry *tss_desc = (struct tss_entry *)&gdt[num];
    
    tss_desc->limit_low    = limit & 0xFFFF;
    tss_desc->base_low     = base & 0xFFFF;
    tss_desc->base_middle  = (base >> 16) & 0xFF;
    tss_desc->access       = 0x89;  // Present, 64-bit TSS (Available)
    tss_desc->granularity  = ((limit >> 16) & 0x0F);
    tss_desc->base_high    = (base >> 24) & 0xFF;
    tss_desc->base_upper   = (base >> 32) & 0xFFFFFFFF;
    tss_desc->reserved     = 0;
}

void gdt_init(void) {
    gp.limit = (sizeof(struct gdt_entry) * 7) - 1;
    gp.base  = (uint64_t)&gdt;

    // 0: Null Descriptor
    gdt_set_gate(0, 0, 0, 0, 0);

    // 1: Kernel Code Segment (selector 0x08)
    // Access: 0x9A = 1001 1010 (Present, Ring 0, Code, Exec/Read)
    // Granularity: 0xAF = 1010 1111 (4KB pages, Long Mode)
    gdt_set_gate(1, 0, 0, 0x9A, 0xAF);

    // 2: Kernel Data Segment (selector 0x10)
    // Access: 0x92 = 1001 0010 (Present, Ring 0, Data, Read/Write)
    gdt_set_gate(2, 0, 0, 0x92, 0xAF);

    // 3: User Data Segment (selector 0x18, use 0x1B for RPL 3)
    // Access: 0xF2 = 1111 0010 (Present, Ring 3, Data, Read/Write)
    gdt_set_gate(3, 0, 0, 0xF2, 0xAF);

    // 4: User Code Segment (selector 0x20, use 0x23 for RPL 3)
    // Access: 0xFA = 1111 1010 (Present, Ring 3, Code, Exec/Read)
    gdt_set_gate(4, 0, 0, 0xFA, 0xAF);

    // Initialize TSS
    for (int i = 0; i < (int)sizeof(kernel_tss); i++) {
        ((uint8_t *)&kernel_tss)[i] = 0;
    }
    kernel_tss.iopb_offset = sizeof(kernel_tss);

    // 5-6: TSS Descriptor (takes 2 slots in 64-bit mode)
    tss_set_gate(5, (uint64_t)&kernel_tss, sizeof(kernel_tss) - 1);

    // Load the GDT
    global_gdt_flush((uint64_t)&gp);
    
    // Load the TSS
    tss_flush();
}

void tss_set_kernel_stack(uint64_t stack) {
    kernel_tss.rsp0 = stack;
}

struct tss *get_tss(void) {
    return &kernel_tss;
}
