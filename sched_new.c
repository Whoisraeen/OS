// sched_new.c - Preemptive Multitasking Scheduler
// Replace the existing sched.c with this implementation

#include "sched.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "console.h"
#include <stddef.h>

// Task array
static task_t tasks[MAX_TASKS];
static uint32_t current_task_idx = 0;
static uint32_t num_tasks = 0;
static int scheduler_enabled = 0;

// Stack size per task (16KB)
#define TASK_STACK_SIZE (4 * 4096)

// External: get HHDM offset
extern uint64_t pmm_get_hhdm_offset(void);

// ========================================================================
// Scheduler Initialization
// ========================================================================

void scheduler_init(void) {
    // Clear all tasks
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
        tasks[i].id = i;
        tasks[i].rsp = 0;
        tasks[i].stack = NULL;
    }

    // Task 0 is the kernel/init task (already running)
    tasks[0].id = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].stack = NULL;  // Uses existing kernel stack
    tasks[0].rsp = 0;       // Will be filled on first context switch

    const char *n = "kernel";
    for (int i = 0; n[i] && i < 31; i++) tasks[0].name[i] = n[i];
    tasks[0].name[31] = '\0';

    num_tasks = 1;
    current_task_idx = 0;
    scheduler_enabled = 0;  // Don't enable yet - need at least 2 tasks

    kprintf("[SCHED] Scheduler initialized (preemptive)\n");
}

// ========================================================================
// Task Creation
// ========================================================================

int task_create(const char *name, void (*entry)(void)) {
    if (num_tasks >= MAX_TASKS) {
        kprintf("[SCHED] Max tasks reached!\n");
        return -1;
    }

    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        kprintf("[SCHED] No free task slots!\n");
        return -1;
    }

    // Allocate stack (4 pages)
    uint64_t stack_phys = (uint64_t)pmm_alloc_page();
    if (stack_phys == 0) {
        kprintf("[SCHED] Failed to allocate task stack!\n");
        return -1;
    }

    // Allocate additional pages for 16KB stack
    for (int i = 1; i < 4; i++) {
        uint64_t page = (uint64_t)pmm_alloc_page();
        if (page == 0) {
            kprintf("[SCHED] Failed to allocate full stack!\n");
            pmm_free_page((void *)stack_phys);
            return -1;
        }
    }

    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t *stack = (uint64_t *)(stack_phys + hhdm);

    // Set up initial stack for context switch
    // Stack layout (grows down):
    // [Top of stack - 16KB]
    // ... empty space ...
    // RIP (entry point)
    // RFLAGS (0x202 - interrupts enabled)
    // CS (0x08 - kernel code segment)
    // ... saved registers (RAX, RBX, RCX, etc.) ...
    // [RSP points here]

    // Start at top of 16KB stack
    uint64_t *sp = (uint64_t *)((uint64_t)stack + TASK_STACK_SIZE - 8);

    // Push iretq frame (reverse order since stack grows down)
    *--sp = 0x10;              // SS (kernel data segment)
    *--sp = (uint64_t)sp + 8;  // RSP (will be fixed)
    *--sp = 0x202;             // RFLAGS (IF=1, reserved bit 1 set)
    *--sp = 0x08;              // CS (kernel code segment)
    *--sp = (uint64_t)entry;   // RIP (entry point)

    // Push all general purpose registers (15 total)
    // Order: R15, R14, R13, R12, R11, R10, R9, R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX
    for (int i = 0; i < 15; i++) {
        *--sp = 0;  // Zero out registers initially
    }

    // Push segment registers
    *--sp = 0x10;  // DS
    *--sp = 0x10;  // ES
    *--sp = 0x10;  // FS
    *--sp = 0x10;  // GS

    // Set up task structure
    tasks[slot].id = slot;
    tasks[slot].state = TASK_READY;
    tasks[slot].stack = stack;
    tasks[slot].rsp = (uint64_t)sp;  // Save current stack pointer

    // Copy name
    for (int i = 0; name[i] && i < 31; i++) tasks[slot].name[i] = name[i];
    tasks[slot].name[31] = '\0';

    num_tasks++;

    kprintf("[SCHED] Created task %d: %s (entry=0x%lx, rsp=0x%lx)\n",
            slot, name, (uint64_t)entry, tasks[slot].rsp);

    // Enable scheduler if we now have 2+ tasks
    if (num_tasks >= 2 && !scheduler_enabled) {
        scheduler_enabled = 1;
        kprintf("[SCHED] Multitasking enabled!\n");
    }

    return slot;
}

// ========================================================================
// Context Switching
// ========================================================================

// This is called from the timer ISR with the interrupted task's RSP
// It returns the new task's RSP to restore
uint64_t scheduler_switch(uint64_t old_rsp) {
    if (!scheduler_enabled || num_tasks < 2) {
        return old_rsp;  // No switching needed
    }

    // Save current task's RSP
    tasks[current_task_idx].rsp = old_rsp;
    tasks[current_task_idx].state = TASK_READY;

    // Find next ready task (round-robin)
    uint32_t next_idx = current_task_idx;
    int attempts = 0;
    do {
        next_idx = (next_idx + 1) % MAX_TASKS;
        attempts++;

        // Check if this task is ready to run
        if (tasks[next_idx].state == TASK_READY || tasks[next_idx].state == TASK_RUNNING) {
            break;
        }

        // Safety check: don't loop forever
        if (attempts >= MAX_TASKS) {
            kprintf("[SCHED] WARNING: No ready tasks found!\n");
            return old_rsp;  // Stay on current task
        }
    } while (1);

    // Switch to new task
    uint32_t old_idx = current_task_idx;
    current_task_idx = next_idx;
    tasks[current_task_idx].state = TASK_RUNNING;

    // Debug output (only occasionally to avoid spam)
    static int switch_count = 0;
    if (switch_count % 100 == 0) {
        kprintf("[SCHED] Switch: %s (PID %d) -> %s (PID %d)\n",
                tasks[old_idx].name, old_idx,
                tasks[current_task_idx].name, current_task_idx);
    }
    switch_count++;

    return tasks[current_task_idx].rsp;
}

// ========================================================================
// Legacy Functions (for compatibility)
// ========================================================================

// Called from timer interrupt (old interface)
void scheduler_tick(void) {
    // This is now a no-op - real scheduling happens in scheduler_switch()
    // which is called from the timer ISR with proper context
}

void task_yield(void) {
    // Trigger a timer interrupt to force a context switch
    __asm__ volatile ("int $0x20");
}

uint32_t task_current_id(void) {
    return current_task_idx;
}

void task_exit(void) {
    tasks[current_task_idx].state = TASK_TERMINATED;
    num_tasks--;

    kprintf("[SCHED] Task %d (%s) terminated\n",
            current_task_idx, tasks[current_task_idx].name);

    // Free task stack
    if (tasks[current_task_idx].stack) {
        uint64_t stack_phys = (uint64_t)tasks[current_task_idx].stack - pmm_get_hhdm_offset();
        pmm_free_page((void *)stack_phys);

        // Free remaining stack pages
        for (int i = 1; i < 4; i++) {
            pmm_free_page((void *)(stack_phys + i * 4096));
        }
    }

    // Force a context switch
    task_yield();

    // Should never return
    for (;;) __asm__ ("hlt");
}

// ========================================================================
// Debugging
// ========================================================================

void scheduler_debug_print_tasks(void) {
    kprintf("\n[SCHED] Task List:\n");
    kprintf("  ID  State   Name            RSP\n");
    kprintf("  --  ------  --------------  ----------------\n");

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            const char *state_str = "UNKNOWN";
            switch (tasks[i].state) {
                case TASK_READY:      state_str = "READY";   break;
                case TASK_RUNNING:    state_str = "RUNNING"; break;
                case TASK_BLOCKED:    state_str = "BLOCKED"; break;
                case TASK_TERMINATED: state_str = "TERM";    break;
                default: break;
            }

            char current = (i == current_task_idx) ? '*' : ' ';
            kprintf("%c %2d  %-7s %-15s 0x%016lx\n",
                    current, i, state_str, tasks[i].name, tasks[i].rsp);
        }
    }

    kprintf("\nTotal tasks: %d, Scheduler: %s\n",
            num_tasks, scheduler_enabled ? "ENABLED" : "DISABLED");
}
