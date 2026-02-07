#ifndef TSS_H
#define TSS_H

#include <stdint.h>

// Task State Segment for x86-64
// Used to store kernel stack pointer for Ring 3 → Ring 0 transitions
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;      // Stack pointer for Ring 0 (kernel)
    uint64_t rsp1;      // Stack pointer for Ring 1 (unused)
    uint64_t rsp2;      // Stack pointer for Ring 2 (unused)
    uint64_t reserved1;
    uint64_t ist1;      // Interrupt Stack Table 1
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;      // I/O Permission Bitmap offset
} tss_t;

// Initialize TSS
void tss_init(void);

// Set the kernel stack for Ring 3 → Ring 0 transitions
void tss_set_kernel_stack(uint64_t stack);

// Get current kernel stack
uint64_t tss_get_kernel_stack(void);

#endif
