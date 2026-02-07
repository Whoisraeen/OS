#include "tss.h"
#include "serial.h"
#include "heap.h"

// Global TSS
static tss_t tss __attribute__((aligned(16)));

// Kernel stack for Ring 3 transitions (8KB)
#define KERNEL_STACK_SIZE 8192
static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

// External: flush TSS (load TR register)
extern void tss_flush(void);

// External pointer for assembly
extern uint64_t kernel_tss_rsp0_ptr;

void tss_init(void) {
    // Clear TSS
    uint8_t *p = (uint8_t *)&tss;
    for (size_t i = 0; i < sizeof(tss_t); i++) {
        p[i] = 0;
    }
    
    // Set kernel stack (top of stack, stack grows down)
    tss.rsp0 = (uint64_t)&kernel_stack[KERNEL_STACK_SIZE];
    
    // Update the assembly pointer used by syscall_entry
    kernel_tss_rsp0_ptr = tss.rsp0;
    
    // Set I/O permission bitmap offset (beyond TSS = no I/O bitmap)
    tss.iopb = sizeof(tss_t);
    
    // Load the TSS into TR register
    tss_flush();
    
    kprintf("[TSS] Initialized, kernel stack at 0x%lx\n", tss.rsp0);
}

void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}

uint64_t tss_get_kernel_stack(void) {
    return tss.rsp0;
}

// Get TSS address (for GDT setup)
tss_t *tss_get(void) {
    return &tss;
}
