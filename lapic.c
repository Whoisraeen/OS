#include "lapic.h"
#include "io.h"
#include "serial.h"
#include "vmm.h"

// MSR for APIC Base
#define MSR_APIC_BASE 0x1B

// Offsets
#define LAPIC_ID        0x020
#define LAPIC_EOI       0x0B0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

static uint64_t lapic_base_addr = 0;

// Read a value from LAPIC register
static uint32_t lapic_read(uint32_t reg) {
    if (lapic_base_addr == 0) return 0;
    return *(volatile uint32_t *)(lapic_base_addr + reg);
}

// Write a value to LAPIC register
static void lapic_write(uint32_t reg, uint32_t value) {
    if (lapic_base_addr == 0) return;
    *(volatile uint32_t *)(lapic_base_addr + reg) = value;
}

void lapic_init(void) {
    // 1. Get LAPIC Base from MSR
    uint64_t apic_msr = rdmsr(MSR_APIC_BASE);
    
    // Physical address is in bits 12-51
    uint64_t phys_base = apic_msr & 0xFFFFF000;
    
    // Map it if not already mapped (we'll just use HHDM + Offset if possible, 
    // or map it to a fixed location).
    // For simplicity, let's assume we can map it to 0xFFFFFFFF_FEE00000 + offset?
    // Actually, VMM usually maps all physical memory in HHDM.
    // But let's be safe and map it explicitly.
    // For now, let's try to map it to 0xFFFF8000FEE00000 (Kernel Space High)
    // Or just 1:1 map it? No, we are in higher half.
    
    // Let's use vmm_map_page. We need a virtual address.
    // Let's just pick one.
    // Note: All CPUs share the same physical address for LAPIC, 
    // but it accesses *their* local APIC.
    
    if (lapic_base_addr == 0) {
        // Map it once (global variable)
        // We can't map it differently for each CPU, they use the same address.
        lapic_base_addr = phys_base + 0xFFFF800000000000; // HHDM offset?
        
        // Map it explicitly to ensure it's accessible and uncached
        // 0x13 = Present | Writable | NoCache (Bit 4)
        vmm_map_page(lapic_base_addr, phys_base, 0x13);
        
        kprintf("[LAPIC] Mapped 0x%lx -> 0x%lx\n", phys_base, lapic_base_addr);
    }
    
    // 2. Enable LAPIC
    // Set Spurious Interrupt Vector to 0xFF (255) and bit 8 (Enable)
    lapic_write(LAPIC_SPURIOUS, 0x1FF);
    
    // 3. Setup Timer
    // Divide by 16 (0x3)
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    
    // Set Init Count (Determine frequency later, for now just a constant)
    // 10,000,000
    lapic_write(LAPIC_TIMER_INIT, 10000000);
    
    // LVT Timer: Periodic (0x20000) | Vector 0x40 (64)
    // Masked? No.
    lapic_write(LAPIC_LVT_TIMER, 0x20040);
    
    kprintf("[LAPIC] Initialized on CPU. Base: 0x%lx\n", lapic_base_addr);
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_id(void) {
    return (lapic_read(LAPIC_ID) >> 24);
}
