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
#include "fd.h"
#include "io.h"
#include "vm_area.h"
#include "elf.h"
#include "string.h"
#include <stdbool.h>

#define MSR_FS_BASE 0xC0000100

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
    tasks[0].fd_table = NULL; // Kernel task has no fd table
    tasks[0].mm = NULL;       // Kernel task has no mm
    tasks[0].parent_pid = 0;
    tasks[0].exit_code = 0;
    tasks[0].tgid = 0;
    tasks[0].tls_base = 0;
    tasks[0].pending_signals = 0;
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

    // IMPORTANT: 16-byte alignment for stack
    // RSP should be 16-byte aligned before CALL (so return addr makes it 8-byte aligned)
    // Interrupt frame pushes SS, RSP, RFLAGS, CS, RIP (5 * 8 = 40 bytes)
    // Then Err, IntNo (2 * 8 = 16 bytes)
    // Then 15 GPRs (15 * 8 = 120 bytes)
    // Then 3 Segs (3 * 8 = 24 bytes)
    // Total = 200 bytes.
    // If stack_top is aligned, (stack_top - 200) is aligned.
    
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
    // Order popped in interrupts.S: FS, ES, DS
    // Order pushed: DS, ES, FS
    *--sp = GDT_KERNEL_DATA; // DS
    *--sp = GDT_KERNEL_DATA; // ES
    *--sp = GDT_KERNEL_DATA; // FS

    tasks[slot].rsp = (uint64_t)sp;
    tasks[slot].id = slot;
    tasks[slot].cr3 = 0; // Kernel task, use current CR3
    tasks[slot].fd_table = NULL;
    tasks[slot].mm = NULL;
    tasks[slot].parent_pid = task_current_id();
    tasks[slot].exit_code = 0;
    tasks[slot].tgid = slot; // Group leader by default
    tasks[slot].tls_base = 0;
    tasks[slot].pending_signals = 0;

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
            // Disable interrupts to prevent deadlock with scheduler ISR
            __asm__ volatile("cli");
            spinlock_acquire(&target_cpu->lock);
            sched_enqueue(target_cpu, &tasks[slot]);
            spinlock_release(&target_cpu->lock);
            __asm__ volatile("sti");

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

int task_create_user(const char *name, const void *elf_data, size_t size, uint32_t parent_pid) {
    // 1. Create task slot WITHOUT enqueuing (entry=NULL, auto_enqueue=false)
    //    This prevents the scheduler from running a half-initialized task
    int slot = task_create_ex(name, NULL, false);
    if (slot < 0) return -1;

    task_t *task = &tasks[slot];
    task->parent_pid = parent_pid;

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

    uint64_t last_stack_phys = 0;
    for (uint64_t addr = user_stack_base; addr < user_stack_top; addr += 4096) {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        if (addr + 4096 >= user_stack_top) last_stack_phys = phys;
        vmm_map_user_page(addr, phys); // Uses current CR3 = user PML4
    }
    
    // Initialize Stack (argc=0, argv=NULL, envp=NULL)
    // We need to write 3 uint64_t zeros at the top of the stack
    if (last_stack_phys) {
        uint64_t *stack_page = (uint64_t *)(last_stack_phys + vmm_get_hhdm_offset());
        // Stack grows down. Top is at offset 4096.
        // We push 3 values:
        // [4088] = 0 (envp terminator?)
        // [4080] = 0 (argv terminator?)
        // [4072] = 0 (argc)
        // RSP points to 4072.
        
        // Actually crt0 does: pop rax (argc).
        // So rsp must point to argc.
        
        stack_page[511] = 0; // envp
        stack_page[510] = 0; // argv
        stack_page[509] = 0; // argc
        
        // Adjust RSP
        user_stack_top -= 24; 
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
    // ...
    // Note: The stack layout here MUST match interrupts.S `isr_common_stub` stack frame
    // [int_no] [err_code] [RIP] [CS] [RFLAGS] [RSP] [SS] ...
    // Wait, the stack we set up in task_create_ex puts:
    // [SS] [RSP] [RFLAGS] [CS] [RIP] [err] [int] [GPRs] [Segs]
    // 
    // In interrupts.S:
    //   push rax ... push r15
    //   push ds ... push gs (if we push gs, but we don't in new code)
    // 
    // Let's verify task_create_ex stack setup vs interrupts.S
    // In task_create_ex:
    // *--sp = GDT_KERNEL_DATA; // DS
    // *--sp = GDT_KERNEL_DATA; // ES
    // *--sp = GDT_KERNEL_DATA; // FS
    
    // In interrupts.S:
    // pop rax; mov fs, ax
    // pop rax; mov es, ax
    // pop rax; mov ds, ax
    
    // Order of pops: FS, ES, DS.
    // Order of pushes should be: DS, ES, FS.
    // In task_create_ex we push: DS, ES, FS. So FS is at top (lowest addr).
    // Pop FS (top) -> Correct.
    
    sp[0] = GDT_USER_DATA; // FS
    sp[1] = GDT_USER_DATA; // ES
    sp[2] = GDT_USER_DATA; // DS

    sp[20] = entry_point;      // RIP
    sp[21] = GDT_USER_CODE;   // CS (User Code, Ring 3)
    sp[22] = 0x202;            // RFLAGS (IF=1)
    sp[23] = user_stack_top;   // RSP (User Stack)
    sp[24] = GDT_USER_DATA;   // SS (User Data, Ring 3)

    // 5. Create file descriptor table with stdio
    task->fd_table = fd_table_create();
    if (task->fd_table) {
        fd_init_stdio(task->fd_table);
    }

    // 5b. Create mm_struct with VMAs for ELF segments and user stack
    task->mm = mm_create();
    if (task->mm) {
        // Parse ELF to register VMAs for loaded segments
        const elf64_header_t *hdr = (const elf64_header_t *)elf_data;
        const uint8_t *file_data = (const uint8_t *)elf_data;
        uint64_t highest_end = 0;

        for (uint16_t i = 0; i < hdr->e_phnum; i++) {
            const elf64_phdr_t *phdr = (const elf64_phdr_t *)
                (file_data + hdr->e_phoff + i * hdr->e_phentsize);
            if (phdr->p_type != PT_LOAD) continue;

            uint64_t seg_start = phdr->p_vaddr & ~0xFFFULL;
            uint64_t seg_end = (phdr->p_vaddr + phdr->p_memsz + 0xFFF) & ~0xFFFULL;
            uint32_t flags = VMA_USER | VMA_READ;
            if (phdr->p_flags & PF_W) flags |= VMA_WRITE;
            if (phdr->p_flags & PF_X) flags |= VMA_EXEC;

            vm_area_t *vma = vma_create(seg_start, seg_end, flags, VMA_TYPE_FILE);
            if (vma) vma_insert(task->mm, vma);

            if (seg_end > highest_end) highest_end = seg_end;
        }

        // Set up heap region (brk) after highest loaded segment
        uint64_t heap_start = (highest_end + 0xFFF) & ~0xFFFULL;
        task->mm->start_brk = heap_start;
        task->mm->brk = heap_start;

        // Register user stack VMA
        vm_area_t *stack_vma = vma_create(user_stack_base, user_stack_top,
            VMA_USER | VMA_READ | VMA_WRITE, VMA_TYPE_ANONYMOUS);
        if (stack_vma) vma_insert(task->mm, stack_vma);
    }

    // 6. Create security context for this process
    security_create_context(slot, parent_pid);

    // 7. NOW enqueue the fully-initialized task
    cpu_t *target_cpu = smp_get_cpu_by_id(task->cpu_id);
    if (target_cpu) {
        kprintf("[SCHED] Enqueuing task %d on CPU %d (lock=%d)\n", slot, task->cpu_id, target_cpu->lock.locked);
        
        // Disable interrupts to prevent deadlock with scheduler ISR
        __asm__ volatile("cli");
        spinlock_acquire(&target_cpu->lock);
        sched_enqueue(target_cpu, task);
        spinlock_release(&target_cpu->lock);
        __asm__ volatile("sti");
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
            // Check if parent exists — if so, keep as zombie for wait()
            task_t *parent = NULL;
            if (current->parent_pid > 0 && current->parent_pid < MAX_TASKS) {
                parent = &tasks[current->parent_pid];
                if (parent->state == TASK_UNUSED) parent = NULL;
            }
            // Wake any blocked thread in same group (for thread_join)
            for (int wi = 0; wi < MAX_TASKS; wi++) {
                if (tasks[wi].state == TASK_BLOCKED && tasks[wi].tgid == current->tgid) {
                    tasks[wi].state = TASK_READY;
                    cpu_t *wcpu = smp_get_cpu_by_id(tasks[wi].cpu_id);
                    if (wcpu) {
                        if (wcpu != cpu) spinlock_acquire(&wcpu->lock);
                        sched_enqueue(wcpu, &tasks[wi]);
                        if (wcpu != cpu) spinlock_release(&wcpu->lock);
                    }
                }
            }
            if (parent) {
                // Zombie: parent exists, keep task for reaping
                // Wake parent if it's blocked (waiting for children)
                if (parent->state == TASK_BLOCKED) {
                    parent->state = TASK_READY;
                    cpu_t *pcpu = smp_get_cpu_by_id(parent->cpu_id);
                    if (pcpu) {
                        if (pcpu != cpu) spinlock_acquire(&pcpu->lock);
                        sched_enqueue(pcpu, parent);
                        if (pcpu != cpu) spinlock_release(&pcpu->lock);
                    }
                }
            } else {
                // Orphan or thread: immediate cleanup
                // Always free per-thread kernel stack
                if (current->stack_base) {
                    uint64_t hhdm = pmm_get_hhdm_offset();
                    uint64_t phys = (uint64_t)current->stack_base - hhdm;
                    pmm_free_pages((void *)phys, (TASK_STACK_SIZE + 4095) / 4096);
                    current->stack_base = NULL;
                }
                // Only free shared resources if last thread in group
                int group_count = 0;
                for (int i = 0; i < MAX_TASKS; i++) {
                    if (i == (int)current->id) continue;
                    if (tasks[i].state != TASK_UNUSED && tasks[i].tgid == current->tgid)
                        group_count++;
                }
                if (group_count == 0) {
                    if (current->fd_table) {
                        fd_table_destroy(current->fd_table);
                        current->fd_table = NULL;
                    }
                    if (current->mm) {
                        mm_destroy(current->mm);
                        current->mm = NULL;
                    }
                    if (current->cr3) {
                        vmm_destroy_user_space(current->cr3);
                        current->cr3 = 0;
                    }
                } else {
                    // Other threads still alive — don't free shared resources
                    current->fd_table = NULL;
                    current->mm = NULL;
                    current->cr3 = 0;
                }
                current->state = TASK_UNUSED;
                spinlock_acquire(&tasks_alloc_lock);
                num_tasks--;
                spinlock_release(&tasks_alloc_lock);
            }
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

    // Update TSS RSP0 for the new task so interrupts/syscalls from Ring 3 work
    if (next->stack_base) {
        cpu->tss.rsp0 = (uint64_t)next->stack_base + TASK_STACK_SIZE;
    } else {
        // Fallback for kernel tasks that might not have a separate stack base set?
        // But task_create_ex sets stack_base.
        // For task 0 (idle), it might not be set?
        // Task 0 uses boot stack?
    }

    // 5. Switch address space if needed
    if (next->cr3) {
        uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if (next->cr3 != current_cr3) {
            __asm__ volatile("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
        }
    }

    // 6. Switch TLS base (FS segment) if set
    if (next->tls_base) {
        wrmsr(MSR_FS_BASE, next->tls_base);
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
    task_exit_code(0);
}

void task_exit_code(int code) {
    cpu_t *cpu = get_cpu();

    spinlock_acquire(&cpu->lock);
    task_t *current = (task_t *)cpu->current_task;
    if (current) {
        kprintf("[SCHED] Task %d (%s) exiting with code %d on CPU %d\n",
                current->id, current->name, code, cpu->cpu_id);
        current->exit_code = code;
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

// Forward declaration
static void reap_child(task_t *child);

// Thread stack region (each thread gets USER_STACK_SIZE with 4KB guard)
#define THREAD_STACK_REGION 0x7FFFFF000000ULL

int task_create_thread(uint64_t entry, uint64_t arg, uint64_t user_stack) {
    uint32_t my_pid = task_current_id();
    task_t *parent = task_get_by_id(my_pid);
    if (!parent) return -1;

    // Allocate task slot
    spinlock_acquire(&tasks_alloc_lock);
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_TERMINATED) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        spinlock_release(&tasks_alloc_lock);
        return -1;
    }
    tasks[slot].state = TASK_READY;
    num_tasks++;
    spinlock_release(&tasks_alloc_lock);

    // Allocate kernel stack
    void *kstack = pmm_alloc_pages((TASK_STACK_SIZE + 4095) / 4096);
    if (!kstack) {
        spinlock_acquire(&tasks_alloc_lock);
        tasks[slot].state = TASK_UNUSED;
        num_tasks--;
        spinlock_release(&tasks_alloc_lock);
        return -1;
    }

    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t kstack_virt = (uint64_t)kstack + hhdm;
    tasks[slot].stack_base = (void *)kstack_virt;

    // If no user stack provided, allocate one in the process address space
    if (user_stack == 0) {
        uint64_t thread_stack_top = THREAD_STACK_REGION - (uint64_t)slot * (USER_STACK_SIZE + 4096);
        uint64_t thread_stack_base = thread_stack_top - USER_STACK_SIZE;
        for (uint64_t addr = thread_stack_base; addr < thread_stack_top; addr += 4096) {
            uint64_t phys = (uint64_t)pmm_alloc_page();
            vmm_map_user_page(addr, phys);
        }
        user_stack = thread_stack_top;
    }

    // Build IRETQ frame on kernel stack
    uint64_t kstack_top = kstack_virt + TASK_STACK_SIZE;
    uint64_t *sp = (uint64_t *)kstack_top;

    *--sp = GDT_USER_DATA;     // SS
    *--sp = user_stack;         // RSP (user stack)
    *--sp = 0x202;              // RFLAGS (IF=1)
    *--sp = GDT_USER_CODE;     // CS
    *--sp = entry;              // RIP

    *--sp = 0; // err_code
    *--sp = 0; // int_no

    // GPRs: R15, R14, R13, R12, R11, R10, R9, R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX
    *--sp = 0;     // R15
    *--sp = 0;     // R14
    *--sp = 0;     // R13
    *--sp = 0;     // R12
    *--sp = 0;     // R11
    *--sp = 0;     // R10
    *--sp = 0;     // R9
    *--sp = 0;     // R8
    *--sp = 0;     // RBP
    *--sp = arg;   // RDI = first argument
    *--sp = 0;     // RSI
    *--sp = 0;     // RDX
    *--sp = 0;     // RCX
    *--sp = 0;     // RBX
    *--sp = 0;     // RAX

    // Segment registers
    *--sp = GDT_USER_DATA; // DS
    *--sp = GDT_USER_DATA; // ES
    *--sp = GDT_USER_DATA; // FS

    tasks[slot].rsp = (uint64_t)sp;
    tasks[slot].id = slot;
    tasks[slot].cr3 = parent->cr3;          // Share address space
    tasks[slot].fd_table = parent->fd_table; // Share fd table
    tasks[slot].mm = parent->mm;            // Share mm_struct
    tasks[slot].parent_pid = parent->id;
    tasks[slot].exit_code = 0;
    tasks[slot].tgid = parent->tgid;         // Same thread group
    tasks[slot].tls_base = 0;
    tasks[slot].pending_signals = 0;

    // Copy name with /T suffix
    int i;
    for (i = 0; parent->name[i] && i < 29; i++) tasks[slot].name[i] = parent->name[i];
    tasks[slot].name[i++] = '/';
    tasks[slot].name[i++] = 'T';
    tasks[slot].name[i] = 0;

    // Assign to CPU (round robin)
    int cpu_count = smp_get_cpu_count();
    uint32_t target_cpu_id = __sync_fetch_and_add(&next_cpu_rr, 1) % cpu_count;
    tasks[slot].cpu_id = target_cpu_id;

    cpu_t *target_cpu = smp_get_cpu_by_id(target_cpu_id);
    if (target_cpu) {
        spinlock_acquire(&target_cpu->lock);
        sched_enqueue(target_cpu, &tasks[slot]);
        spinlock_release(&target_cpu->lock);
    }

    kprintf("[SCHED] Created Thread %d in group %d on CPU %d\n", slot, parent->tgid, target_cpu_id);
    return slot;
}

int task_thread_join(uint32_t tid) {
    if (tid == 0 || tid >= MAX_TASKS) return -1;

    uint32_t my_pid = task_current_id();
    task_t *me = task_get_by_id(my_pid);
    if (!me) return -1;

    task_t *target = &tasks[tid];

    // Must be in the same thread group
    if (target->tgid != me->tgid) return -1;

    while (1) {
        if (target->state == TASK_TERMINATED) {
            int code = target->exit_code;
            // Reap the thread
            reap_child(target);
            return code;
        }
        if (target->state == TASK_UNUSED) return -1;

        // Block until thread exits
        task_block();
    }
}

// Fork: create child process with COW
int task_fork(registers_t *parent_regs) {
    uint32_t my_pid = task_current_id();
    task_t *parent = task_get_by_id(my_pid);
    if (!parent || !parent->cr3) return -1;

    // Allocate task slot
    spinlock_acquire(&tasks_alloc_lock);
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_TERMINATED) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        spinlock_release(&tasks_alloc_lock);
        return -1;
    }
    tasks[slot].state = TASK_READY;
    num_tasks++;
    spinlock_release(&tasks_alloc_lock);

    // Clone page tables with COW
    uint64_t child_cr3 = vmm_fork_user_space(parent->cr3);
    if (!child_cr3) {
        spinlock_acquire(&tasks_alloc_lock);
        tasks[slot].state = TASK_UNUSED;
        num_tasks--;
        spinlock_release(&tasks_alloc_lock);
        return -1;
    }

    // Allocate kernel stack for child
    void *kstack = pmm_alloc_pages((TASK_STACK_SIZE + 4095) / 4096);
    if (!kstack) {
        vmm_destroy_user_space(child_cr3);
        spinlock_acquire(&tasks_alloc_lock);
        tasks[slot].state = TASK_UNUSED;
        num_tasks--;
        spinlock_release(&tasks_alloc_lock);
        return -1;
    }

    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t kstack_virt = (uint64_t)kstack + hhdm;
    tasks[slot].stack_base = (void *)kstack_virt;

    // Copy the parent's register frame (interrupt stack frame) to child's kernel stack
    // This makes the child resume at the same point as the parent
    uint64_t kstack_top = kstack_virt + TASK_STACK_SIZE;
    registers_t *child_regs = (registers_t *)(kstack_top - sizeof(registers_t));
    memcpy(child_regs, parent_regs, sizeof(registers_t));

    // Child returns 0 from fork
    child_regs->rax = 0;

    tasks[slot].rsp = (uint64_t)child_regs;
    tasks[slot].id = slot;
    tasks[slot].cr3 = child_cr3;
    tasks[slot].parent_pid = parent->id;
    tasks[slot].exit_code = 0;
    tasks[slot].tgid = slot;  // New process group
    tasks[slot].tls_base = parent->tls_base;
    tasks[slot].pending_signals = 0;

    // Clone fd table
    if (parent->fd_table) {
        tasks[slot].fd_table = fd_table_create();
        if (tasks[slot].fd_table) {
            memcpy(tasks[slot].fd_table, parent->fd_table, sizeof(struct fd_table));
        }
    } else {
        tasks[slot].fd_table = NULL;
    }

    // Clone mm_struct
    if (parent->mm) {
        tasks[slot].mm = mm_clone(parent->mm);
    } else {
        tasks[slot].mm = NULL;
    }

    // Copy name
    int i;
    for (i = 0; parent->name[i] && i < 31; i++) tasks[slot].name[i] = parent->name[i];
    tasks[slot].name[i] = 0;

    // Assign to CPU
    int cpu_count = smp_get_cpu_count();
    uint32_t target_cpu_id = __sync_fetch_and_add(&next_cpu_rr, 1) % cpu_count;
    tasks[slot].cpu_id = target_cpu_id;

    cpu_t *target_cpu = smp_get_cpu_by_id(target_cpu_id);
    if (target_cpu) {
        spinlock_acquire(&target_cpu->lock);
        sched_enqueue(target_cpu, &tasks[slot]);
        spinlock_release(&target_cpu->lock);
    }

    kprintf("[SCHED] Forked PID %d -> %d (COW)\n", parent->id, slot);
    return slot;
}

// Helper: reap a terminated child (free its resources, mark UNUSED)
static void reap_child(task_t *child) {
    if (child->fd_table) {
        fd_table_destroy(child->fd_table);
        child->fd_table = NULL;
    }
    if (child->mm) {
        mm_destroy(child->mm);
        child->mm = NULL;
    }
    if (child->cr3) {
        vmm_destroy_user_space(child->cr3);
        child->cr3 = 0;
    }
    if (child->stack_base) {
        uint64_t hhdm = pmm_get_hhdm_offset();
        uint64_t phys = (uint64_t)child->stack_base - hhdm;
        pmm_free_pages((void *)phys, (TASK_STACK_SIZE + 4095) / 4096);
        child->stack_base = NULL;
    }
    child->state = TASK_UNUSED;
    spinlock_acquire(&tasks_alloc_lock);
    num_tasks--;
    spinlock_release(&tasks_alloc_lock);
}

int task_wait(int *status) {
    uint32_t my_pid = task_current_id();

    while (1) {
        int has_children = 0;

        // Scan for children
        for (int i = 1; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_UNUSED) continue;
            if (tasks[i].parent_pid != my_pid) continue;

            has_children = 1;

            if (tasks[i].state == TASK_TERMINATED) {
                // Found a zombie child — reap it
                int child_pid = tasks[i].id;
                if (status) *status = tasks[i].exit_code;
                reap_child(&tasks[i]);
                return child_pid;
            }
        }

        if (!has_children) return -1; // No children at all

        // Children exist but none terminated yet — block
        task_block();
        // Woken up: loop and check again
    }
}

int task_waitpid(uint32_t pid, int *status) {
    if (pid == 0 || pid >= MAX_TASKS) return -1;

    uint32_t my_pid = task_current_id();
    task_t *child = &tasks[pid];

    // Verify it's our child
    if (child->state == TASK_UNUSED || child->parent_pid != my_pid)
        return -1;

    while (1) {
        if (child->state == TASK_TERMINATED) {
            if (status) *status = child->exit_code;
            reap_child(child);
            return (int)pid;
        }

        if (child->state == TASK_UNUSED) return -1; // Already gone

        // Child still running — block
        task_block();
    }
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
