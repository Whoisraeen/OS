#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>

// Maximum number of tasks
#define MAX_TASKS 16

// Task states
typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_TERMINATED
} task_state_t;

// Task context (saved registers)
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} task_context_t;

// Task Control Block
typedef struct {
    uint32_t id;
    task_state_t state;
    uint64_t *stack;        // Kernel stack base
    uint64_t rsp;           // Current stack pointer
    char name[32];
} task_t;

// Initialize scheduler
void scheduler_init(void);

// Create a new task
int task_create(const char *name, void (*entry)(void));

// Called on each timer tick
void scheduler_tick(void);

// Yield CPU voluntarily
void task_yield(void);

// Get current task ID
uint32_t task_current_id(void);

// Terminate current task
void task_exit(void);

// Context switching function (called from timer ISR)
// Takes old RSP, returns new RSP
uint64_t scheduler_switch(uint64_t old_rsp);

// Debug function to print all tasks
void scheduler_debug_print_tasks(void);

#endif
