#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "idt.h"
#include "limine/limine.h"
#include "spinlock.h"

// Forward declaration
struct task_t;

// Maximum number of CPUs supported
#define MAX_CPUS 32

// Per-CPU Data Structure
    // This structure holds all information specific to a single core
    typedef struct {
        uint64_t syscall_scratch; // Scratch space for syscall handler (RSP saving)
        uint32_t lapic_id;      // Local APIC ID
        uint32_t cpu_id;        // Our sequential CPU ID (0, 1, 2...)
    
    // Per-CPU GDT and TSS
    struct gdt_entry gdt[7]; // Kernel Code, Kernel Data, User Data, User Code, TSS
    struct tss tss;
    
    // Pointer to current running task on this CPU
    void *current_task; // (struct task_t *)
    
    // Scheduler Run Queue (Linked List)
    struct task_t *run_queue_head;
    struct task_t *run_queue_tail;
    
    // Per-CPU Scheduler Lock
    spinlock_t lock;
    
    // Idle Task (fallback when queue is empty)
    void *idle_task; 
    
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

// Get CPU structure by ID
cpu_t *smp_get_cpu_by_id(uint32_t id);

#endif
