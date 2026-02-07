#include "user.h"
#include "pmm.h"
#include "vmm.h"
#include "gdt.h"
#include "console.h"
#include "serial.h"

// User code will be loaded at this virtual address
#define USER_CODE_VADDR 0x400000
#define USER_STACK_VADDR 0x800000

// External assembly function to jump to Ring 3
extern void jump_to_usermode(uint64_t entry, uint64_t stack);

// The actual user program code (will be copied to user pages)
// This is position-independent code that uses syscalls
static const uint8_t user_program_code[] = {
    // mov rax, 1          ; SYS_WRITE
    0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,
    // mov rdi, 1          ; fd = stdout  
    0x48, 0xC7, 0xC7, 0x01, 0x00, 0x00, 0x00,
    // lea rsi, [rip+msg]  ; buffer (msg is at end of code)
    0x48, 0x8D, 0x35, 0x1B, 0x00, 0x00, 0x00,
    // mov rdx, 30         ; length
    0x48, 0xC7, 0xC2, 0x1E, 0x00, 0x00, 0x00,
    // int 0x80            ; syscall
    0xCD, 0x80,
    // mov rax, 0          ; SYS_EXIT
    0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
    // mov rdi, 42         ; exit code
    0x48, 0xC7, 0xC7, 0x2A, 0x00, 0x00, 0x00,
    // int 0x80            ; syscall
    0xCD, 0x80,
    // jmp $               ; infinite loop (should never reach)
    0xEB, 0xFE,
    // Message: "Hello from Ring 3 userspace!\n"
    'H', 'e', 'l', 'l', 'o', ' ', 'f', 'r', 'o', 'm', ' ',
    'R', 'i', 'n', 'g', ' ', '3', ' ', 'u', 's', 'e', 'r',
    's', 'p', 'a', 'c', 'e', '!', '\n', '\0'
};

void user_program_entry(void) {
    // Placeholder - actual code is in user_program_code[]
}

void start_user_process(void) {
    console_printf("\n [USER] Starting TRUE Ring 3 process...\n");
    kprintf("[USER] Starting TRUE Ring 3 process...\n");
    
    // Allocate physical pages for user code and stack
    uint64_t code_phys = (uint64_t)pmm_alloc_page();
    uint64_t stack_phys = (uint64_t)pmm_alloc_page();
    
    if (code_phys == 0 || stack_phys == 0) {
        console_printf(" [USER] Failed to allocate user pages!\n");
        return;
    }
    
    // Map these pages as USER pages (Ring 3 accessible)
    vmm_map_user_page(USER_CODE_VADDR, code_phys);
    vmm_map_user_page(USER_STACK_VADDR, stack_phys);
    
    // Flush TLB by reloading CR3
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    
    // Copy user program code to the mapped page
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint8_t *code_virt = (uint8_t *)(code_phys + hhdm);
    
    for (size_t i = 0; i < sizeof(user_program_code); i++) {
        code_virt[i] = user_program_code[i];
    }
    
    // Set kernel stack in TSS for interrupt returns
    uint64_t kernel_rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(kernel_rsp));
    tss_set_kernel_stack(kernel_rsp);
    
    // User stack pointer (top of stack page)
    uint64_t user_stack = USER_STACK_VADDR + 4096 - 8;
    
    console_printf(" [USER] Code at virt 0x%lx (phys 0x%lx)\n", USER_CODE_VADDR, code_phys);
    console_printf(" [USER] Stack at virt 0x%lx (phys 0x%lx)\n", user_stack, stack_phys);
    console_printf(" [USER] Jumping to Ring 3...\n\n");
    kprintf("[USER] Jumping to Ring 3 at 0x%lx\n", USER_CODE_VADDR);
    
    // Jump to Ring 3!
    jump_to_usermode(USER_CODE_VADDR, user_stack);
    
    // Should never return
    console_printf(" [USER] ERROR: Returned from Ring 3!\n");
}
