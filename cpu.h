#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "idt.h"
#include "limine/limine.h"

// Maximum number of CPUs supported
#define MAX_CPUS 32

// Per-CPU Data Structure
// This structure holds all information specific to a single core
typedef struct {
    uint32_t lapic_id;      // Local APIC ID
    uint32_t cpu_id;        // Our sequential CPU ID (0, 1, 2...)
    
    // Per-CPU GDT and TSS
    struct gdt_entry gdt[7]; // Kernel Code, Kernel Data, User Data, User Code, TSS
    struct tss tss;
    
    // Pointer to current running task on this CPU
    void *current_task;
    
    // Scheduler runqueue for this CPU (simplified for now)
    // In a real OS, this would be a priority queue
    
    // Helper to store GS base
    uint64_t self;          // Points to this structure
} cpu_t;

// Get current CPU structure (using GS segment)
static inline cpu_t *get_cpu(void) {
    cpu_t *cpu;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(cpu));
    return cpu;
}

// Initialize SMP
void smp_init(void);

// Get number of active CPUs
int smp_get_cpu_count(void);

#endif
