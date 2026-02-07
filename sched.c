#include "sched.h"
#include "pmm.h"
#include "serial.h"
#include "console.h"

// Task array
static task_t tasks[MAX_TASKS];
static uint32_t current_task = 0;
static uint32_t num_tasks = 0;
static int scheduler_enabled = 0;

// Stack size per task (4 pages = 16KB)
#define TASK_STACK_SIZE (4 * 4096)

// External: get HHDM offset for stack allocation
extern uint64_t pmm_get_hhdm_offset(void);

void scheduler_init(void) {
    // Clear all tasks
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
        tasks[i].id = i;
    }
    
    // Task 0 is the kernel/init task (already running)
    tasks[0].id = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].stack = NULL;  // Uses existing kernel stack
    tasks[0].rsp = 0;
    
    // Copy name
    const char *n = "kernel";
    for (int i = 0; n[i] && i < 31; i++) tasks[0].name[i] = n[i];
    tasks[0].name[31] = '\0';
    
    num_tasks = 1;
    current_task = 0;
    scheduler_enabled = 1;
    
    kprintf("[SCHED] Scheduler initialized\n");
}

int task_create(const char *name, void (*entry)(void)) {
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
    
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t *stack = (uint64_t *)(stack_phys + hhdm);
    
    // Set up initial stack for context switch
    // Stack grows down, so start at top
    uint64_t *sp = (uint64_t *)((uint64_t)stack + 4096 - 8);
    
    // Push initial context (iretq frame + registers)
    // This mirrors what the ISR stub expects
    *--sp = 0x10;           // SS (kernel data)
    *--sp = (uint64_t)sp;   // RSP (will be updated)
    *--sp = 0x202;          // RFLAGS (IF=1)
    *--sp = 0x08;           // CS (kernel code)
    *--sp = (uint64_t)entry; // RIP (entry point)
    
    // Push dummy registers (r15-rax = 15 registers)
    for (int i = 0; i < 15; i++) {
        *--sp = 0;
    }
    
    // Set up task
    tasks[slot].id = slot;
    tasks[slot].state = TASK_READY;
    tasks[slot].stack = stack;
    tasks[slot].rsp = (uint64_t)sp;
    
    // Copy name
    for (int i = 0; name[i] && i < 31; i++) tasks[slot].name[i] = name[i];
    tasks[slot].name[31] = '\0';
    
    num_tasks++;
    
    kprintf("[SCHED] Created task %d: %s\n", slot, name);
    return slot;
}

// Simple round-robin scheduler
void scheduler_tick(void) {
    if (!scheduler_enabled || num_tasks <= 1) {
        return;
    }
    
    // Find next ready task
    uint32_t next = current_task;
    for (int i = 0; i < MAX_TASKS; i++) {
        next = (next + 1) % MAX_TASKS;
        if (tasks[next].state == TASK_READY || tasks[next].state == TASK_RUNNING) {
            break;
        }
    }
    
    if (next == current_task) {
        return;  // No other task to run
    }
    
    // Context switch
    tasks[current_task].state = TASK_READY;
    uint32_t old = current_task;
    current_task = next;
    tasks[current_task].state = TASK_RUNNING;
    
    // The actual context switch happens in the ISR return
    // We save/restore RSP in the task structure
    // For now, this is a simplified implementation
    (void)old;
}

void task_yield(void) {
    __asm__ volatile ("int $0x20");  // Trigger timer interrupt
}

uint32_t task_current_id(void) {
    return current_task;
}

void task_exit(void) {
    tasks[current_task].state = TASK_TERMINATED;
    num_tasks--;
    kprintf("[SCHED] Task %d terminated\n", current_task);
    
    // Yield to let scheduler pick next task
    task_yield();
    
    // Should never return
    for (;;) __asm__ ("hlt");
}
