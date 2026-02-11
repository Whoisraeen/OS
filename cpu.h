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
// IMPORTANT: Field order matters! Assembly in interrupts.S uses hardcoded
// GS-relative offsets. If you reorder fields, update the offsets below AND
// in syscall_entry (interrupts.S).
//
// Offset map (all packed structs, no padding between first 5 fields):
//   0:  self              (8 bytes)  — get_cpu() reads gs:[0]
//   8:  syscall_scratch   (8 bytes)  — syscall_entry uses gs:[8]
//  16:  lapic_id          (4 bytes)
//  20:  cpu_id            (4 bytes)
//  24:  gdt[7]            (56 bytes)
//  80:  tss               (104 bytes, packed)
//  84:  tss.rsp0          — syscall_entry uses gs:[84]
typedef struct {
    // MUST be at offset 0: get_cpu() reads gs:[0] to get self pointer
    uint64_t self;

    // Stack for AP startup (trampoline) - Offset 8
    uint64_t startup_stack_top;

    // Scratch space for syscall handler - Offset 16
    uint64_t syscall_scratch;

    uint32_t lapic_id;      // Local APIC ID
    uint32_t cpu_id;        // Our sequential CPU ID (0, 1, 2...)

    // Per-CPU GDT and TSS
    struct gdt_entry gdt[7];
    struct tss tss;

    // Pointer to current running task on this CPU
    struct task_t *current_task;

    // Scheduler Run Queue (Linked List)
    struct task_t *run_queue_head;
    struct task_t *run_queue_tail;

    // Per-CPU Scheduler Lock
    spinlock_t lock;

    // Idle Task (fallback when queue is empty)
    void *idle_task;
} cpu_t;

// Compile-time offset verification for assembly compatibility
_Static_assert(__builtin_offsetof(cpu_t, self) == 0,
    "cpu_t.self must be at offset 0 for get_cpu()");
_Static_assert(__builtin_offsetof(cpu_t, startup_stack_top) == 8,
    "cpu_t.startup_stack_top must be at offset 8 for smp_ap_trampoline");
_Static_assert(__builtin_offsetof(cpu_t, syscall_scratch) == 16,
    "cpu_t.syscall_scratch must be at offset 16 for syscall_entry");

// Get current CPU structure (using GS segment)
// GS_BASE points to cpu_t, and self (at offset 0) points back to itself
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
