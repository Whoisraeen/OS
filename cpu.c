#include "cpu.h"
#include "limine/limine.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "io.h"
#include "lapic.h"
#include "syscall.h"

// Request SMP information from Limine
__attribute__((used, section(".requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0
};

// Global array of CPU structures (allocated dynamically later)
static cpu_t *cpus = NULL;
static int cpu_count = 0;

// GDT entries (copied from gdt.c)
// We need to replicate GDT setup for each core
static void setup_cpu_gdt(cpu_t *cpu) {
    gdt_setup_cpu(cpu->gdt, &cpu->tss);
}

// Entry point for Application Processors (APs)
// Limine jumps here for each core
void smp_ap_entry(struct limine_smp_info *info) {
    cpu_t *cpu = (cpu_t *)info->extra_argument;

    // 0. Switch to kernel page tables (Limine's AP tables lack our mappings)
    vmm_switch();

    // 1. Load GDT
    struct gdt_ptr gdt_ptr;
    gdt_ptr.limit = sizeof(struct gdt_entry) * 7 - 1;
    gdt_ptr.base = (uint64_t)&cpu->gdt;
    
    extern void global_gdt_flush(uint64_t);
    global_gdt_flush((uint64_t)&gdt_ptr);
    
    // 2. Load IDT (shared for now)
    extern struct gdt_ptr idtp; // Defined in idt.c
    __asm__ volatile ("lidt %0" : : "m"(idtp));
    
    // 3. Load TSS
    extern void tss_flush(void);
    tss_flush();
    
    // 4. Setup GS Base to point to cpu_t
    // This allows us to use get_cpu()
    uint64_t gs_base = (uint64_t)cpu;
    
    // Write MSR_GS_BASE (0xC0000101) using helper
    wrmsr(0xC0000101, gs_base);
    wrmsr(0xC0000102, 0); // KERNEL_GS_BASE = 0
    
    // 5. Initialize LAPIC (and Timer)
    lapic_init();

    // 5b. Initialize SYSCALL/SYSRET MSRs (per-CPU)
    syscall_init();
    
    // Start LAPIC Timer on AP (using calibration from BSP)
    lapic_timer_start();
    
    // Disable SMAP/SMEP (Bits 20, 21 of CR4)
    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~(1 << 20); // SMEP
    cr4 &= ~(1 << 21); // SMAP
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    
    // 6. Signal we are ready
    kprintf("[SMP] CPU %d (LAPIC %d) Online!\n", cpu->cpu_id, cpu->lapic_id);
    
    // 7. Enable Interrupts (so Timer can fire)
    __asm__ volatile ("sti");

    // 8. Enter Idle Loop (Wait for scheduler)
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void smp_init(void) {
    struct limine_smp_response *smp = smp_request.response;
    
    if (smp == NULL) {
        kprintf("[SMP] Request failed (Limine didn't respond)\n");
        // Fallback: Initialize BSP only
        cpu_count = 1;
        cpus = kmalloc(sizeof(cpu_t));
        if (!cpus) return;

        cpus[0].cpu_id = 0;
        cpus[0].lapic_id = 0; // Assume 0 for BSP
        cpus[0].self = (uint64_t)&cpus[0];
        setup_cpu_gdt(&cpus[0]);
        
        // Setup GS for BSP
        uint64_t gs_base = (uint64_t)&cpus[0];
        wrmsr(0xC0000101, gs_base);
        wrmsr(0xC0000102, 0);
        
        // Initialize LAPIC
        lapic_init();
        return;
    }
    
    cpu_count = smp->cpu_count;
    kprintf("[SMP] Detected %d CPUs\n", cpu_count);
    
    // Allocate CPU structures
    cpus = kmalloc(sizeof(cpu_t) * cpu_count);
    if (!cpus) {
        kprintf("[SMP] Failed to allocate CPU structures\n");
        return;
    }
    
    // DEBUG: Verify offsets for assembly
    uint64_t base = (uint64_t)&cpus[0];
    uint64_t scratch = (uint64_t)&cpus[0].syscall_scratch;
    uint64_t rsp0 = (uint64_t)&cpus[0].tss.rsp0;
    kprintf("[DEBUG] cpu_t offsets: scratch=%lu, tss.rsp0=%lu\n", 
            scratch - base, rsp0 - base);
    
    // Initialize BSP (Bootstrap Processor - CPU 0)
    // We are running on it right now.
    struct limine_smp_info *bsp_info = NULL;
    
    for (uint64_t i = 0; i < smp->cpu_count; i++) {
        struct limine_smp_info *info = smp->cpus[i];
        
        cpus[i].cpu_id = i;
        cpus[i].lapic_id = info->lapic_id;
        cpus[i].self = (uint64_t)&cpus[i];
        
        // Setup GDT/TSS for this core
        setup_cpu_gdt(&cpus[i]);
        
        // Allocate startup stack for AP (4KB is enough for init)
        // We use PMM directly to get a page
        void *stack_page = pmm_alloc_page();
        if (stack_page) {
            // Stack grows down from top of page + HHDM
            cpus[i].startup_stack_top = (uint64_t)stack_page + 0xFFFF800000000000ULL + 4096;
        } else {
            kprintf("[SMP] Failed to allocate stack for CPU %d\n", i);
        }

        // Pass cpu_t pointer to the AP
        info->extra_argument = (uint64_t)&cpus[i];
        
        if (info->lapic_id == smp->bsp_lapic_id) {
            bsp_info = info;
            kprintf("[SMP] CPU %d is BSP\n", i);
            
            // CRITICAL: Reload GDT and TSS for BSP to use per-CPU structures
            // Otherwise TR points to old kernel_tss, but scheduler updates cpus[i].tss
            struct gdt_ptr gdt_ptr;
            gdt_ptr.limit = sizeof(struct gdt_entry) * 7 - 1;
            gdt_ptr.base = (uint64_t)&cpus[i].gdt;
            
            extern void global_gdt_flush(uint64_t);
            global_gdt_flush((uint64_t)&gdt_ptr);
            
            extern void tss_flush(void);
            tss_flush();
            
            // Setup GS for BSP immediately (AFTER GDT flush which resets GS)
            uint64_t gs_base = (uint64_t)&cpus[i];
            wrmsr(0xC0000101, gs_base);
            wrmsr(0xC0000102, 0); // KERNEL_GS_BASE = 0 (User Base)
            
            // Initialize LAPIC on BSP too
            lapic_init();
            
            kprintf("[SMP] BSP GDT/TSS reloaded.\n");
            
        } else {
            // Wake up AP
            // Limine handles the IPI sequence (INIT-SIPI-SIPI)
            // We set goto_address to our assembly trampoline which sets up RSP
            extern void smp_ap_trampoline(struct limine_smp_info *info);
            info->goto_address = smp_ap_trampoline;
        }
    }
    
    kprintf("[SMP] Initialization complete. All cores waking up...\n");
}

int smp_get_cpu_count(void) {
    if (cpu_count <= 0) return 1; 
    return cpu_count;
}

cpu_t *smp_get_cpu_by_id(uint32_t id) {
    if (id >= (uint32_t)cpu_count) return NULL;
    return &cpus[id];
}
