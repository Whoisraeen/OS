#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IDT Entry Structure
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

// IDT Pointer Structure
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// Interrupt Frame (pushed by CPU/ISR stub)
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

// Initialize IDT
void idt_init(void);

#endif
