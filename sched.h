#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>

#define MAX_TASKS 32
#define TASK_STACK_SIZE (16 * 1024)

// CPU Registration State (what is pushed by ISR)
// Note: This must match the stack layout in interrupts.S
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) registers_t;

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_TERMINATED
} task_state_t;

typedef struct {
    uint32_t id;
    char name[32];
    task_state_t state;
    
    uint64_t rsp;        // Kernel stack pointer (saved context)
    void *stack_base;    // Base of allocated kernel stack
    uint64_t cr3;        // Page table (for future process isolation)
    
    // Wakeup time for sleeping tasks
    uint64_t wakeup_ticks;
} task_t;

// API
void scheduler_init(void);
int task_create(const char *name, void (*entry)(void));
void task_exit(void);
void task_yield(void);
uint32_t task_current_id(void);

// Called by Timer ISR (returns new RSP)
uint64_t scheduler_switch(registers_t *regs);

#endif
