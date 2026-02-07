#include "idt.h"
#include "pic.h"
#include "serial.h"
#include <stddef.h>

struct idt_entry idt[256];
struct idt_ptr idtp;

// External ISR stubs (assembly)
extern void *isr_stub_table[];

// External Keyboard Handler (from keyboard.c)
extern void keyboard_handler(struct interrupt_frame *frame);

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low    = (base & 0xFFFF);
    idt[num].offset_middle = (base >> 16) & 0xFFFF;
    idt[num].offset_high   = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector      = sel;
    idt[num].ist           = 0;
    idt[num].flags         = flags;
    idt[num].zero          = 0;
}

// Debugging globals
extern uint32_t *fb_ptr;
extern uint64_t fb_width;
extern uint64_t fb_height;

// Syscall handler
extern uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3);

void isr_handler(struct interrupt_frame *frame) {
    // Syscall (int 0x80 = vector 128)
    if (frame->int_no == 128) {
        // Syscall convention: rax=syscall#, rdi=arg1, rsi=arg2, rdx=arg3
        // Return value goes in rax
        frame->rax = syscall_handler(frame->rax, frame->rdi, frame->rsi, frame->rdx);
        return;
    }
    
    // Exceptions (0-31)
    if (frame->int_no < 32) {
        // Print exception info to serial for debugging
        extern void kprintf(const char *fmt, ...);
        kprintf("\n*** EXCEPTION %lu ***\n", frame->int_no);
        kprintf("Error code: 0x%lx\n", frame->err_code);
        kprintf("RIP: 0x%lx\n", frame->rip);
        kprintf("CS:  0x%lx\n", frame->cs);
        kprintf("RSP: 0x%lx\n", frame->rsp);
        kprintf("SS:  0x%lx\n", frame->ss);
        
        // EXCEPTION! TURN SCREEN RED
        if (fb_ptr) {
            for (size_t i = 0; i < fb_width * fb_height; i++) {
                fb_ptr[i] = 0xFFFF0000; // Red
            }
        }
        // Halt
        for (;;) __asm__("hlt");
    }

    // NOTE: IRQ 0 (Timer, Vector 32) is now handled entirely in assembly
    // with context switching. Do not handle it here.

    // IRQ 1 is mapped to Vector 33 (32 + 1)
    if (frame->int_no == 33) {
        keyboard_handler(frame);
    }

    // IRQ 12 is mapped to Vector 44 (Mouse)
    if (frame->int_no == 44) {
        static int irq12_count = 0;
        irq12_count++;
        if (irq12_count <= 5) {
            kprintf("[IDT] IRQ12 received! count=%d\n", irq12_count);
        }
        extern void mouse_handler(void);
        mouse_handler();
    }

    // Send EOI for all IRQs (vectors 32-47)
    // Timer (32) sends its own EOI in assembly, but this won't hurt
    if (frame->int_no >= 32 && frame->int_no <= 47) {
        pic_send_eoi(frame->int_no - 32);
    }
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

    // Load IDT
    __asm__ volatile ("lidt %0" : : "m"(idtp));
    
    // Enable interrupts? Not yet. we don't handle them properly.
    // __asm__ volatile ("sti"); 
}
