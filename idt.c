#include "idt.h"
#include "pic.h"
#include "lapic.h"
#include "serial.h"
#include "sched.h"
#include "timer.h"
#include "cpu.h"
#include "console.h" // For panic BSOD
#include "klog.h"    // For panic dump
#include <stddef.h>
#include <stdarg.h>

struct idt_entry idt[256];
struct idt_ptr idtp;

// External ISR stubs (assembly)
extern void *isr_stub_table[];

// External Keyboard Handler (from keyboard.c)
extern void keyboard_handler(struct interrupt_frame *frame);

// External Mouse Handler (from mouse.c)
extern void mouse_handler(void);

// AHCI Handler
extern void ahci_isr(void);

// E1000 Handler
extern void e1000_isr(void);

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

void panic(const char *fmt, ...) {
    // Disable interrupts
    __asm__ volatile ("cli");
    
    // Force enable console for BSOD
    console_set_enabled(1);
    
    // Blue Background (BSOD)
    if (fb_ptr) {
        for (size_t i = 0; i < fb_width * fb_height; i++)
            fb_ptr[i] = 0xFF0000AA; // Blue
    }
    
    // Reset cursor and colors
    console_set_colors(0xFFFFFFFF, 0xFF0000AA);
    console_clear();
    
    kprintf("\n  *** KERNEL PANIC ***\n\n");
    
    // Since kprintf doesn't support va_list, we just print the format string
    // and hope it's descriptive enough. 
    // Ideally we should implement vkprintf.
    kprintf("Reason: %s\n", fmt);
    
    kprintf("\n\nSystem Halted.\n");
    
    // Dump klog
    kprintf("\n--- Kernel Log Dump ---\n");
    klog_dump();
    
    for (;;) __asm__("hlt");
}

// Waiters for IRQs
static task_t *irq_waiters[256] = {0};

void irq_register_waiter(int irq, task_t *task) {
    if (irq < 0 || irq >= 256) return;
    irq_waiters[irq] = task;
}

void irq_notify_waiter(int irq) {
    if (irq < 0 || irq >= 256) return;
    task_t *waiter = irq_waiters[irq];
    if (waiter) {
        task_unblock(waiter);
        irq_waiters[irq] = NULL; // One-shot
    }
}

uint64_t isr_handler(struct interrupt_frame *frame) {
    // ---- Timer IRQ (Vector 32) — Preemptive scheduling ----
    if (frame->int_no == 32) {
        timer_tick();
        if (lapic_is_ioapic_mode())
            lapic_eoi();
        else
            pic_send_eoi(0);

        // Notify any task waiting for timer (e.g. sleep)
        irq_notify_waiter(0); // IRQ 0

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

        // User-mode page fault: terminate the task
        if (frame->cs & 3) {
            console_set_enabled(1);
            kprintf("\n[PAGE FAULT] User task %u killed — Addr: 0x%lx, IP: 0x%lx, Err: 0x%lx\n",
                    task_current_id(), faulting_addr, frame->rip, frame->err_code);
            task_exit();
            return (uint64_t)frame;
        }

        // Kernel page fault: unrecoverable, panic
        console_set_enabled(1);
        if (fb_ptr) {
            for (size_t i = 0; i < fb_width * fb_height; i++)
                fb_ptr[i] = 0xFFFF0000; // Red
        }
        console_set_colors(0xFFFFFFFF, 0xFFFF0000);
        console_clear();
        kprintf("*** KERNEL PANIC (PAGE FAULT) ***\n");
        kprintf("Address: 0x%lx\n", faulting_addr);
        kprintf("Error:   0x%lx\n", frame->err_code);
        kprintf("RIP:     0x%lx\n", frame->rip);
        klog_dump();
        for (;;) __asm__("hlt");
    }

    // ---- Other exceptions (0-31, except 14 handled above) ----
    if (frame->int_no < 32) {
        const char *name = (frame->int_no < 32) ? exception_names[frame->int_no] : "Unknown";
        
        // User-mode exception: terminate the task
        if (frame->cs & 3) {
            console_set_enabled(1);
            kprintf("\n[EXCEPTION] %s (0x%lx) at 0x%lx — task %u killed\n",
                    name, frame->int_no, frame->rip, task_current_id());
            task_exit();
            return (uint64_t)frame;
        }

        // Kernel exception: panic
        console_set_enabled(1);
        if (fb_ptr) {
            for (size_t i = 0; i < fb_width * fb_height; i++)
                fb_ptr[i] = 0xFFFF0000;
        }
        console_set_colors(0xFFFFFFFF, 0xFFFF0000);
        console_clear();
        kprintf("*** KERNEL PANIC (%s) ***\n", name);
        kprintf("Error code: 0x%lx\n", frame->err_code);
        kprintf("RIP: 0x%lx\n", frame->rip);
        klog_dump();
        for (;;) __asm__("hlt");
    }

    // ---- IRQ 1 (Keyboard, Vector 33) ----
    if (frame->int_no == 33) {
        // keyboard_handler(frame); // Disabled for userspace driver
        irq_notify_waiter(1);
    }

    // ---- IRQ 12 (Mouse, Vector 44) ----
    if (frame->int_no == 44) {
        // mouse_handler(); // Disabled for userspace driver
        irq_notify_waiter(12);
    }

    // ---- AHCI MSI (Vector 46) ----
    if (frame->int_no == 46) {
        ahci_isr();
        if (lapic_is_ioapic_mode()) lapic_eoi();
        return (uint64_t)frame;
    }

    // ---- E1000 MSI (Vector 47) ----
    if (frame->int_no == 47) {
        e1000_isr();
        if (lapic_is_ioapic_mode()) lapic_eoi();
        return (uint64_t)frame;
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
