#include "sched.h"
#include "pmm.h"
#include "serial.h"
#include "console.h"
#include "idt.h"
#include <stdbool.h>

static task_t tasks[MAX_TASKS];
static uint32_t current_task_id = 0;
static uint32_t num_tasks = 0;
static bool scheduler_ready = false;

// Global used by assembly to check if we need to switch
uint64_t next_rsp = 0;

void scheduler_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
    }
    
    // Create Kernel Task (Task 0)
    // It is already running, so we don't need a stack allocated for it yet,
    // (Wait, we DO need a place to save its state when we switch AWAY from it)
    // But initially, its state is "running on current stack".
    
    tasks[0].id = 0;
    tasks[0].state = TASK_RUNNING;
    // Name
    const char *n = "kernel_main";
    for(int i=0; n[i]; i++) tasks[0].name[i] = n[i];
    tasks[0].name[11] = 0;
    
    tasks[0].cr3 = 0; // Use current CR3 (kernel)
    
    current_task_id = 0;
    num_tasks = 1;
    scheduler_ready = true;
    
    kprintf("[SCHED] Scheduler Initialized. Main task ID: 0\n");
}

int task_create(const char *name, void (*entry)(void)) {
    // Find free slot
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) { // Start at 1
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_TERMINATED) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        kprintf("[SCHED] No free task slots\n");
        return -1;
    }
    
    // Allocate stack
    void *stack = pmm_alloc_pages((TASK_STACK_SIZE + 4095) / 4096);
    if (!stack) {
        kprintf("[SCHED] OOM allocating stack\n");
        return -1;
    }
    
    // We need virtual address for stack.
    // Assuming identity map coverage is sufficient or using the returned HHDM address
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
    kprintf("[SCHED] Created Task %d (%s)\n", slot, name);
    return slot;
}

uint32_t task_current_id(void) {
    return current_task_id;
}

// Function called by assembly to Switch Tasks
// Returns new RSP to load
uint64_t scheduler_switch(registers_t *regs) {
    if (!scheduler_ready) return (uint64_t)regs;
    
    // 1. Update current task's state
    tasks[current_task_id].rsp = (uint64_t)regs;
    
    // 2. Select next task (Round Robin)
    int next = -1;
    for (int i = 1; i <= MAX_TASKS; i++) { // Search all other tasks
        int idx = (current_task_id + i) % MAX_TASKS;
        if (tasks[idx].state == TASK_READY || tasks[idx].state == TASK_RUNNING) {
            next = idx;
            break;
        }
    }
    
    // If no other task is ready, return current RSP
    if (next == -1 || next == (int)current_task_id) {
        return (uint64_t)regs; 
    }
    
    // 3. Switch Task
    tasks[current_task_id].state = TASK_READY;
    current_task_id = next;
    tasks[current_task_id].state = TASK_RUNNING;
    
    // 4. Update TSS RSP0 (for Ring 3 -> Ring 0 transitions)
    if (tasks[current_task_id].stack_base) {
        uint64_t kstack_top = (uint64_t)tasks[current_task_id].stack_base + TASK_STACK_SIZE;
        extern uint64_t kernel_tss_rsp0_ptr; 
        kernel_tss_rsp0_ptr = kstack_top;
    }
    
    // 5. Return new RSP
    // kprintf("[SCHED] Switch to %d\n", current_task_id);
    return tasks[current_task_id].rsp;
}

void task_exit(void) {
    kprintf("[SCHED] Task %d exiting\n", current_task_id);
    tasks[current_task_id].state = TASK_TERMINATED;
    
    // Force yield
    __asm__ volatile("int $32");
    // Should not return
    for(;;) __asm__("hlt");
}

void task_yield(void) {
    __asm__ volatile("int $32");
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
    kprintf("  Current Task: %d\n", current_task_id);
}
