#include "idt.h"
#include "pic.h"
#include "lapic.h"
#include "serial.h"
#include "sched.h"
#include "timer.h"
#include "cpu.h"
#include <stddef.h>

struct idt_entry idt[256];
struct idt_ptr idtp;

// External ISR stubs (assembly)
extern void *isr_stub_table[];

// External Keyboard Handler (from keyboard.c)
extern void keyboard_handler(struct interrupt_frame *frame);

// External Mouse Handler (from mouse.c)
extern void mouse_handler(void);

// Debugging globals
extern uint32_t *fb_ptr;
extern uint64_t fb_width;
extern uint64_t fb_height;

// Syscall handler
extern uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                 struct interrupt_frame *regs);

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low    = (base & 0xFFFF);
    idt[num].offset_middle = (base >> 16) & 0xFFFF;
    idt[num].offset_high   = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector      = sel;
    idt[num].ist           = 0;
    idt[num].flags         = flags;
    idt[num].zero          = 0;
}

// Exception names for debugging
static const char *exception_names[] = {
    "Division Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound Range", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 FP Exception", "Alignment Check", "Machine Check", "SIMD FP Exception",
    "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved"
};

uint64_t isr_handler(struct interrupt_frame *frame) {
    // ---- Timer IRQ (Vector 32) — Preemptive scheduling ----
    if (frame->int_no == 32) {
        timer_tick();
        if (lapic_is_ioapic_mode())
            lapic_eoi();
        else
            pic_send_eoi(0);

        // Call scheduler — may return a different task's RSP
        uint64_t new_rsp = scheduler_switch((registers_t *)frame);
        return new_rsp;
    }

    // ---- Yield interrupt (Vector 0x40) — Voluntary context switch ----
    if (frame->int_no == YIELD_VECTOR) {
        uint64_t new_rsp = scheduler_switch((registers_t *)frame);
        return new_rsp;
    }

    // ---- Syscall (int 0x80 = Vector 128) ----
    if (frame->int_no == 128) {
        frame->rax = syscall_handler(frame->rax, frame->rdi, frame->rsi, frame->rdx, frame);
        return (uint64_t)frame;
    }

    // ---- Page Fault (Vector 14) ----
    if (frame->int_no == 14) {
        uint64_t faulting_addr;
        __asm__ volatile("mov %%cr2, %0" : "=r"(faulting_addr));

        // Try demand paging / COW handling for user-mode faults
        if (frame->cs & 3) {
            extern int vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code);
            if (vmm_handle_page_fault(faulting_addr, frame->err_code)) {
                return (uint64_t)frame; // Handled — resume execution
            }
        }

        kprintf("\n*** PAGE FAULT ***\n");
        kprintf("Address: 0x%lx\n", faulting_addr);
        kprintf("Error:   0x%lx (", frame->err_code);
        if (frame->err_code & 1) kprintf("present ");
        if (frame->err_code & 2) kprintf("write ");
        else kprintf("read ");
        if (frame->err_code & 4) kprintf("user ");
        else kprintf("kernel ");
        if (frame->err_code & 8) kprintf("reserved-write ");
        if (frame->err_code & 16) kprintf("instruction-fetch ");
        kprintf(")\n");
        kprintf("RIP: 0x%lx  CS: 0x%lx\n", frame->rip, frame->cs);
        kprintf("RSP: 0x%lx  SS: 0x%lx\n", frame->rsp, frame->ss);

        // User-mode page fault: terminate the task
        if (frame->cs & 3) {
            kprintf("[PAGE FAULT] User process fault — terminating task %u\n",
                    task_current_id());
            task_exit();
            return (uint64_t)frame;
        }

        // Kernel page fault: unrecoverable, panic
        kprintf("[PAGE FAULT] KERNEL PANIC — halting\n");
        kprintf("RAX=0x%lx RBX=0x%lx RCX=0x%lx RDX=0x%lx\n",
                frame->rax, frame->rbx, frame->rcx, frame->rdx);
        kprintf("RSI=0x%lx RDI=0x%lx RBP=0x%lx\n",
                frame->rsi, frame->rdi, frame->rbp);
        if (fb_ptr) {
            for (size_t i = 0; i < fb_width * fb_height; i++)
                fb_ptr[i] = 0xFFFF0000;
        }
        for (;;) __asm__("hlt");
    }

    // ---- Other exceptions (0-31, except 14 handled above) ----
    if (frame->int_no < 32) {
        const char *name = (frame->int_no < 32) ? exception_names[frame->int_no] : "Unknown";
        kprintf("\n*** EXCEPTION %lu: %s ***\n", frame->int_no, name);
        kprintf("Error code: 0x%lx\n", frame->err_code);
        kprintf("RIP: 0x%lx  CS: 0x%lx\n", frame->rip, frame->cs);
        kprintf("RSP: 0x%lx  SS: 0x%lx\n", frame->rsp, frame->ss);
        kprintf("RAX=0x%lx RBX=0x%lx RCX=0x%lx RDX=0x%lx\n",
                frame->rax, frame->rbx, frame->rcx, frame->rdx);

        // User-mode exception: terminate the task
        if (frame->cs & 3) {
            kprintf("[EXCEPTION] User process fault — terminating task %u\n",
                    task_current_id());
            task_exit();
            return (uint64_t)frame;
        }

        // Kernel exception: panic
        if (fb_ptr) {
            for (size_t i = 0; i < fb_width * fb_height; i++)
                fb_ptr[i] = 0xFFFF0000;
        }
        for (;;) __asm__("hlt");
    }

    // ---- IRQ 1 (Keyboard, Vector 33) ----
    if (frame->int_no == 33) {
        keyboard_handler(frame);
    }

    // ---- IRQ 12 (Mouse, Vector 44) ----
    if (frame->int_no == 44) {
        mouse_handler();
    }

    // ---- Spurious vector (0xFF = 255) — no EOI needed ----
    if (frame->int_no == 0xFF) {
        return (uint64_t)frame;
    }

    // ---- Send EOI for IRQs (vectors 33-47, timer already handled above) ----
    if (frame->int_no >= 33 && frame->int_no <= 47) {
        if (lapic_is_ioapic_mode())
            lapic_eoi();
        else
            pic_send_eoi(frame->int_no - 32);
    }

    return (uint64_t)frame; // No context switch for regular interrupts
}


void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint64_t)&idt;

    // Set up ISRs (0-31 are exceptions, 32-47 are IRQs, others syscalls/spurious)
    for (int i = 0; i < 256; i++) {
        // 0x8E = Present, Ring 0, Interrupt Gate
        idt_set_gate(i, (uint64_t)isr_stub_table[i], 0x08, 0x8E);
    }

    // Syscall gate (vector 128 = 0x80) must be DPL=3 so Ring 3 can call it
    // 0xEE = Present, Ring 3, Interrupt Gate
    idt_set_gate(128, (uint64_t)isr_stub_table[128], 0x08, 0xEE);

    // Yield gate (vector 0x40) — DPL=3 so user processes can yield via int 0x40
    idt_set_gate(YIELD_VECTOR, (uint64_t)isr_stub_table[YIELD_VECTOR], 0x08, 0xEE);

    // Load IDT
    __asm__ volatile ("lidt %0" : : "m"(idtp));
}
