#include "sched.h"
#include "pmm.h"
#include "serial.h"
#include "console.h"
#include "idt.h"
#include "cpu.h"
#include "spinlock.h"
#include "elf.h"
#include "vmm.h"
#include "security.h"
#include "gdt.h"
#include <stdbool.h>

static task_t tasks[MAX_TASKS];
static spinlock_t tasks_alloc_lock = {0}; // Lock for allocating from 'tasks' array
static uint32_t num_tasks = 0;
static bool scheduler_ready = false;
static uint32_t next_cpu_rr = 0; // Round-robin CPU selector

// Helper: Enqueue task to CPU's run queue
static void sched_enqueue(cpu_t *cpu, task_t *task) {
    // Assumes cpu->lock is held
    task->next = NULL;
    if (cpu->run_queue_tail) {
        cpu->run_queue_tail->next = task;
        cpu->run_queue_tail = task;
    } else {
        cpu->run_queue_head = task;
        cpu->run_queue_tail = task;
    }
}

// Helper: Dequeue task from CPU's run queue
static task_t *sched_dequeue(cpu_t *cpu) {
    // Assumes cpu->lock is held
    task_t *task = cpu->run_queue_head;
    if (task) {
        cpu->run_queue_head = task->next;
        if (!cpu->run_queue_head) {
            cpu->run_queue_tail = NULL;
        }
        task->next = NULL;
    }
    return task;
}

void scheduler_init(void) {
    // Initialize tasks pool
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
    }

    spinlock_init(&tasks_alloc_lock);

    // Initialize locks for all CPUs
    int count = smp_get_cpu_count();
    for (int i = 0; i < count; i++) {
        cpu_t *c = smp_get_cpu_by_id(i);
        if (c) {
            spinlock_init(&c->lock);
            c->run_queue_head = NULL;
            c->run_queue_tail = NULL;
            c->current_task = NULL;
        }
    }

    // Create Kernel Task (Task 0) for BSP — represents the idle loop in _start
    tasks[0].id = 0;
    tasks[0].state = TASK_RUNNING;
    const char *n = "kernel_main";
    for(int i=0; n[i]; i++) tasks[0].name[i] = n[i];
    tasks[0].name[11] = 0;
    tasks[0].cr3 = 0; // Use current CR3 (kernel)
    tasks[0].cpu_id = 0;

    num_tasks = 1;
    scheduler_ready = true;

    // Set BSP current task
    cpu_t *cpu = get_cpu();
    if (cpu) {
        cpu->current_task = &tasks[0];
    }

    kprintf("[SCHED] SMP Scheduler Initialized (Per-CPU Queues).\n");
}

// Internal task creation with option to not auto-enqueue
// When auto_enqueue=false, caller is responsible for enqueuing after setup
static int task_create_ex(const char *name, void (*entry)(void), bool auto_enqueue) {
    spinlock_acquire(&tasks_alloc_lock);

    // Find free slot in global pool
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) { // Start at 1
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_TERMINATED) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        spinlock_release(&tasks_alloc_lock);
        kprintf("[SCHED] No free task slots\n");
        return -1;
    }

    // Reserve slot
    tasks[slot].state = TASK_READY;
    num_tasks++;

    spinlock_release(&tasks_alloc_lock);

    // Allocate stack
    void *stack = pmm_alloc_pages((TASK_STACK_SIZE + 4095) / 4096);
    if (!stack) {
        kprintf("[SCHED] OOM allocating stack\n");
        // Fix: properly release the slot on failure
        spinlock_acquire(&tasks_alloc_lock);
        tasks[slot].state = TASK_UNUSED;
        num_tasks--;
        spinlock_release(&tasks_alloc_lock);
        return -1;
    }

    // Virtual address for stack
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t stack_virt = (uint64_t)stack + hhdm;
    tasks[slot].stack_base = (void*)stack_virt;

    // Setup stack frame for IRETQ
    uint64_t stack_top = stack_virt + TASK_STACK_SIZE;
    uint64_t *sp = (uint64_t *)stack_top;

    *--sp = GDT_KERNEL_DATA; // SS (Kernel Data)
    *--sp = stack_top;       // RSP (this stack)
    *--sp = 0x202;           // RFLAGS (IF=1)
    *--sp = GDT_KERNEL_CODE; // CS (Kernel Code)
    *--sp = (uint64_t)entry; // RIP

    *--sp = 0; // err_code
    *--sp = 0; // int_no

    // GPRs (R15..RAX) — 15 registers
    for(int i=0; i<15; i++) *--sp = 0;

    // Segment registers (DS, ES, FS)
    *--sp = GDT_KERNEL_DATA; // DS
    *--sp = GDT_KERNEL_DATA; // ES
    *--sp = GDT_KERNEL_DATA; // FS

    tasks[slot].rsp = (uint64_t)sp;
    tasks[slot].id = slot;
    tasks[slot].cr3 = 0; // Kernel task, use current CR3

    for(int i=0; name[i] && i<31; i++) tasks[slot].name[i] = name[i];
    tasks[slot].name[31] = 0;

    // Assign to CPU (Round Robin)
    int cpu_count = smp_get_cpu_count();
    uint32_t target_cpu_id = __sync_fetch_and_add(&next_cpu_rr, 1) % cpu_count;
    tasks[slot].cpu_id = target_cpu_id;

    // Only enqueue if auto_enqueue is true
    if (auto_enqueue) {
        cpu_t *target_cpu = smp_get_cpu_by_id(target_cpu_id);
        if (target_cpu) {
            spinlock_acquire(&target_cpu->lock);
            sched_enqueue(target_cpu, &tasks[slot]);
            spinlock_release(&target_cpu->lock);
            kprintf("[SCHED] Created Task %d (%s) on CPU %d\n", slot, name, target_cpu_id);
        } else {
            kprintf("[SCHED] Error: Target CPU %d not found!\n", target_cpu_id);
        }
    }

    return slot;
}

int task_create(const char *name, void (*entry)(void)) {
    return task_create_ex(name, entry, true);
}

int task_create_user(const char *name, const void *elf_data, size_t size) {
    // 1. Create task slot WITHOUT enqueuing (entry=NULL, auto_enqueue=false)
    //    This prevents the scheduler from running a half-initialized task
    int slot = task_create_ex(name, NULL, false);
    if (slot < 0) return -1;

    task_t *task = &tasks[slot];

    // 2. Create VMM context (PML4)
    task->cr3 = vmm_create_user_pml4();
    if (!task->cr3) {
        kprintf("[SCHED] Failed to create PML4 for task %d\n", slot);
        spinlock_acquire(&tasks_alloc_lock);
        task->state = TASK_UNUSED;
        num_tasks--;
        spinlock_release(&tasks_alloc_lock);
        return -1;
    }

    // 3. Load ELF into user address space
    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(task->cr3) : "memory");

    uint64_t entry_point = elf_load_user(elf_data, size);

    // Allocate User Stack
    uint64_t user_stack_top = USER_STACK_TOP;
    uint64_t user_stack_base = user_stack_top - USER_STACK_SIZE;

    for (uint64_t addr = user_stack_base; addr < user_stack_top; addr += 4096) {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        vmm_map_page(addr, phys, 0x07); // User | RW | Present
    }

    // Restore CR3
    __asm__ volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");

    if (entry_point == 0) {
        kprintf("[SCHED] Failed to load ELF for task %d\n", slot);
        spinlock_acquire(&tasks_alloc_lock);
        task->state = TASK_UNUSED;
        num_tasks--;
        spinlock_release(&tasks_alloc_lock);
        return -1;
    }

    // 4. Patch the register frame for user-mode IRETQ
    uint64_t *sp = (uint64_t *)task->rsp;

    // Patch segment registers for Ring 3
    sp[0] = GDT_USER_DATA; // FS
    sp[1] = GDT_USER_DATA; // ES
    sp[2] = GDT_USER_DATA; // DS

    sp[20] = entry_point;      // RIP
    sp[21] = GDT_USER_CODE;   // CS (User Code, Ring 3)
    sp[22] = 0x202;            // RFLAGS (IF=1)
    sp[23] = user_stack_top;   // RSP (User Stack)
    sp[24] = GDT_USER_DATA;   // SS (User Data, Ring 3)

    // 5. Create security context for this process
    security_create_context(slot, 0); // parent=kernel

    // 6. NOW enqueue the fully-initialized task
    cpu_t *target_cpu = smp_get_cpu_by_id(task->cpu_id);
    if (target_cpu) {
        spinlock_acquire(&target_cpu->lock);
        sched_enqueue(target_cpu, task);
        spinlock_release(&target_cpu->lock);
    }

    kprintf("[SCHED] Created User Task %d (%s) Entry=0x%lx\n", slot, name, entry_point);
    return slot;
}

uint32_t task_current_id(void) {
    cpu_t *cpu = get_cpu();
    if (cpu && cpu->current_task) {
        return ((task_t*)cpu->current_task)->id;
    }
    return 0;
}

// Called by timer ISR / yield to switch tasks
// Returns new RSP to load (may be same as input if no switch)
uint64_t scheduler_switch(registers_t *regs) {
    if (!scheduler_ready) return (uint64_t)regs;

    cpu_t *cpu = get_cpu();
    if (!cpu) return (uint64_t)regs;

    spinlock_acquire(&cpu->lock);

    task_t *current = (task_t *)cpu->current_task;

    // 1. Save current state and re-queue if running
    if (current) {
        current->rsp = (uint64_t)regs;
        if (current->state == TASK_RUNNING) {
            current->state = TASK_READY;
            sched_enqueue(cpu, current);
        } else if (current->state == TASK_BLOCKED) {
            cpu->current_task = NULL;
        } else if (current->state == TASK_TERMINATED) {
            cpu->current_task = NULL;
            // Deferred cleanup: free user address space and kernel stack
            // Safe because we're about to switch to a different stack
            if (current->cr3) {
                vmm_destroy_user_space(current->cr3);
                current->cr3 = 0;
            }
            if (current->stack_base) {
                uint64_t hhdm = pmm_get_hhdm_offset();
                uint64_t phys = (uint64_t)current->stack_base - hhdm;
                pmm_free_pages((void *)phys, (TASK_STACK_SIZE + 4095) / 4096);
                current->stack_base = NULL;
            }
            current->state = TASK_UNUSED;
            spinlock_acquire(&tasks_alloc_lock);
            num_tasks--;
            spinlock_release(&tasks_alloc_lock);
        }
    }

    // 2. Select next task
    task_t *next = sched_dequeue(cpu);

    if (!next) {
        // No tasks to run — stay on current context (idle)
        if (current && current->state == TASK_READY) {
            // We just enqueued current, dequeue it back
            next = sched_dequeue(cpu);
        }
        if (!next) {
            cpu->current_task = NULL;
            spinlock_release(&cpu->lock);
            return (uint64_t)regs;
        }
    }

    // 3. Switch to next task
    next->state = TASK_RUNNING;
    cpu->current_task = next;

    // 4. Update TSS RSP0 (kernel stack for when this task takes an interrupt)
    if (next->stack_base) {
        uint64_t kstack_top = (uint64_t)next->stack_base + TASK_STACK_SIZE;
        cpu->tss.rsp0 = kstack_top;
    }

    // 5. Switch address space if needed
    if (next->cr3) {
        uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if (next->cr3 != current_cr3) {
            __asm__ volatile("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
        }
    }

    spinlock_release(&cpu->lock);
    return next->rsp;
}

task_t *task_get_by_id(uint32_t id) {
    if (id >= MAX_TASKS) return NULL;
    if (tasks[id].state == TASK_UNUSED) return NULL;
    return &tasks[id];
}

void task_block(void) {
    cpu_t *cpu = get_cpu();
    if (!cpu) return;

    spinlock_acquire(&cpu->lock);
    task_t *current = (task_t *)cpu->current_task;
    if (current) {
        current->state = TASK_BLOCKED;
    }
    spinlock_release(&cpu->lock);

    // Yield — scheduler_switch will NOT re-enqueue a BLOCKED task
    __asm__ volatile("int $0x40");
}

void task_unblock(task_t *task) {
    if (!task || task->state != TASK_BLOCKED) return;

    cpu_t *target_cpu = smp_get_cpu_by_id(task->cpu_id);
    if (!target_cpu) return;

    spinlock_acquire(&target_cpu->lock);
    task->state = TASK_READY;
    sched_enqueue(target_cpu, task);
    spinlock_release(&target_cpu->lock);
}

void task_exit(void) {
    cpu_t *cpu = get_cpu();

    spinlock_acquire(&cpu->lock);
    task_t *current = (task_t *)cpu->current_task;
    if (current) {
        kprintf("[SCHED] Task %d (%s) exiting on CPU %d\n",
                current->id, current->name, cpu->cpu_id);
        current->state = TASK_TERMINATED;
    }
    spinlock_release(&cpu->lock);

    // Force yield — scheduler_switch will not re-enqueue a TERMINATED task
    __asm__ volatile("int $0x40");
    // Should not return
    for(;;) __asm__("hlt");
}

void task_yield(void) {
    __asm__ volatile("int $0x40");
}

void scheduler_debug_print_tasks(void) {
    kprintf("\n[SCHED] Task List (Global Pool):\n");
    kprintf("  ID   State    CPU  Name\n");
    kprintf("  --   -----    ---  ----\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            const char *state_str = "UNKNOWN";
            switch(tasks[i].state) {
                case TASK_READY: state_str = "READY"; break;
                case TASK_RUNNING: state_str = "RUNNING"; break;
                case TASK_SLEEPING: state_str = "SLEEPING"; break;
                case TASK_BLOCKED: state_str = "BLOCKED"; break;
                case TASK_TERMINATED: state_str = "DEAD"; break;
                default: break;
            }
            kprintf("  %2d   %-8s %3d  %s\n", tasks[i].id, state_str, tasks[i].cpu_id, tasks[i].name);
        }
    }
}
