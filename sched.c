#include "sched.h"
#include "pmm.h"
#include "serial.h"
#include "console.h"
#include "idt.h"
#include "cpu.h"
#include "spinlock.h"
#include <stdbool.h>

static task_t tasks[MAX_TASKS];
static uint32_t num_tasks = 0;
static bool scheduler_ready = false;
static spinlock_t scheduler_lock = {0};

// Global used by assembly to check if we need to switch
uint64_t next_rsp = 0;

void scheduler_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
    }
    
    // Create Kernel Task (Task 0) for BSP
    tasks[0].id = 0;
    tasks[0].state = TASK_RUNNING;
    // Name
    const char *n = "kernel_main";
    for(int i=0; n[i]; i++) tasks[0].name[i] = n[i];
    tasks[0].name[11] = 0;
    
    tasks[0].cr3 = 0; // Use current CR3 (kernel)
    
    num_tasks = 1;
    scheduler_ready = true;
    
    // Set BSP current task
    cpu_t *cpu = get_cpu();
    if (cpu) {
        cpu->current_task = &tasks[0];
    }
    
    kprintf("[SCHED] Scheduler Initialized. Main task ID: 0\n");
}

int task_create(const char *name, void (*entry)(void)) {
    spinlock_acquire(&scheduler_lock);
    
    // Find free slot
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) { // Start at 1
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_TERMINATED) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        spinlock_release(&scheduler_lock);
        kprintf("[SCHED] No free task slots\n");
        return -1;
    }
    
    // Allocate stack
    void *stack = pmm_alloc_pages((TASK_STACK_SIZE + 4095) / 4096);
    if (!stack) {
        spinlock_release(&scheduler_lock);
        kprintf("[SCHED] OOM allocating stack\n");
        return -1;
    }
    
    // We need virtual address for stack.
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t stack_virt = (uint64_t)stack + hhdm;
    
    tasks[slot].stack_base = (void*)stack_virt;
    
    // Setup stack frame for "iretq" behavior when switching TO this task
    uint64_t *sp = (uint64_t *)(stack_virt + TASK_STACK_SIZE);
    
    // Registers_t layout (matches interrupts.S pop order)
    // ss, rsp, rflags, cs, rip, err, int, rax, rbx, rcx, rdx, rsi, rdi, rbp, r8-r15
    
    // Calculate stack top again to be sure
    uint64_t stack_top = stack_virt + TASK_STACK_SIZE;
    sp = (uint64_t *)stack_top;

    *--sp = 0x10;          // SS (Kernel Data)
    *--sp = stack_top;     // RSP (this stack)
    *--sp = 0x202;         // RFLAGS (IF=1)
    *--sp = 0x08;          // CS (Kernel Code)
    *--sp = (uint64_t)entry; // RIP
    
    *--sp = 0; // err_code
    *--sp = 0; // int_no
    
    // Segment registers (GS, FS, ES, DS)
    *--sp = 0x10; // GS
    *--sp = 0x10; // FS
    *--sp = 0x10; // ES
    *--sp = 0x10; // DS
    
    // GPRs
    for(int i=0; i<15; i++) *--sp = 0;
    
    tasks[slot].rsp = (uint64_t)sp;
    tasks[slot].state = TASK_READY;
    tasks[slot].id = slot;
    // Copy name
    for(int i=0; name[i] && i<31; i++) tasks[slot].name[i] = name[i];
    tasks[slot].name[31] = 0;
    
    num_tasks++;
    spinlock_release(&scheduler_lock);
    
    kprintf("[SCHED] Created Task %d (%s)\n", slot, name);
    return slot;
}

uint32_t task_current_id(void) {
    cpu_t *cpu = get_cpu();
    if (cpu && cpu->current_task) {
        return ((task_t*)cpu->current_task)->id;
    }
    return 0;
}

// Function called by assembly to Switch Tasks
// Returns new RSP to load
uint64_t scheduler_switch(registers_t *regs) {
    if (!scheduler_ready) return (uint64_t)regs;
    
    spinlock_acquire(&scheduler_lock);
    
    cpu_t *cpu = get_cpu();
    task_t *current = (task_t *)cpu->current_task;
    
    // 1. Update current task's state (if valid)
    if (current) {
        current->rsp = (uint64_t)regs;
        if (current->state == TASK_RUNNING) {
            current->state = TASK_READY;
        }
    }
    
    // 2. Select next task (Round Robin)
    // Start search from (current_id + 1)
    int start_idx = current ? current->id + 1 : 0;
    int next_slot = -1;
    
    for (int i = 0; i < MAX_TASKS; i++) {
        int idx = (start_idx + i) % MAX_TASKS;
        if (tasks[idx].state == TASK_READY) {
            next_slot = idx;
            break;
        }
    }
    
    // If no other task is ready
    if (next_slot == -1) {
        if (current && current->state == TASK_READY) {
            // Just continue with current task
            current->state = TASK_RUNNING;
            spinlock_release(&scheduler_lock);
            return current->rsp;
        } else {
            // No tasks available (or current died). 
            // If we have a current task, we might be forced to run it?
            // If current was terminated, we have NOTHING to run.
            // Return to caller (idle loop) if possible.
            spinlock_release(&scheduler_lock);
            return (uint64_t)regs;
        }
    }
    
    // 3. Switch Task
    task_t *next_task = &tasks[next_slot];
    next_task->state = TASK_RUNNING;
    cpu->current_task = next_task;
    
    // 4. Update TSS RSP0 (for Ring 3 -> Ring 0 transitions)
    if (next_task->stack_base) {
        uint64_t kstack_top = (uint64_t)next_task->stack_base + TASK_STACK_SIZE;
        // Update per-CPU TSS
        cpu->tss.rsp0 = kstack_top;
    }
    
    // 5. Return new RSP
    spinlock_release(&scheduler_lock);
    return next_task->rsp;
}

void task_exit(void) {
    spinlock_acquire(&scheduler_lock);
    cpu_t *cpu = get_cpu();
    task_t *current = (task_t *)cpu->current_task;
    
    if (current) {
        kprintf("[SCHED] Task %d exiting on CPU %d\n", current->id, cpu->cpu_id);
        current->state = TASK_TERMINATED;
    }
    spinlock_release(&scheduler_lock);
    
    // Force yield
    __asm__ volatile("int $0x40"); // Use SMP Scheduler Vector (64)
    // Should not return
    for(;;) __asm__("hlt");
}

void task_yield(void) {
    __asm__ volatile("int $0x40"); // Use SMP Scheduler Vector (64)
}

void scheduler_debug_print_tasks(void) {
    kprintf("\n[SCHED] Task List:\n");
    kprintf("  ID   State    Name\n");
    kprintf("  --   -----    ----\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            const char *state_str = "UNKNOWN";
            switch(tasks[i].state) {
                case TASK_READY: state_str = "READY"; break;
                case TASK_RUNNING: state_str = "RUNNING"; break;
                case TASK_SLEEPING: state_str = "SLEEPING"; break;
                case TASK_TERMINATED: state_str = "DEAD"; break;
                default: break;
            }
            kprintf("  %2d   %-8s %s\n", tasks[i].id, state_str, tasks[i].name);
        }
    }
}

