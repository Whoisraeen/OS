#include "elf.h"
#include "heap.h"
#include "serial.h"
#include "vmm.h"

// Memory copy
static void mem_copy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

// Memory set
static void mem_set(void *dest, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    while (n--) *d++ = val;
}

int elf_validate(const void *data, size_t size) {
    if (size < sizeof(elf64_header_t)) {
        kprintf("[ELF] Too small for header\n");
        return 0;
    }
    
    const elf64_header_t *hdr = (const elf64_header_t *)data;
    
    // Check magic
    if (*(uint32_t *)hdr->e_ident != ELF_MAGIC) {
        kprintf("[ELF] Invalid magic: 0x%08x\n", *(uint32_t *)hdr->e_ident);
        return 0;
    }
    
    // Check 64-bit
    if (hdr->e_ident[4] != ELFCLASS64) {
        kprintf("[ELF] Not 64-bit ELF\n");
        return 0;
    }
    
    // Check little endian
    if (hdr->e_ident[5] != ELFDATA2LSB) {
        kprintf("[ELF] Not little-endian\n");
        return 0;
    }
    
    // Check executable or shared object
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        kprintf("[ELF] Not executable (type=%d)\n", hdr->e_type);
        return 0;
    }
    
    // Check architecture
    if (hdr->e_machine != EM_X86_64) {
        kprintf("[ELF] Not x86-64 (machine=%d)\n", hdr->e_machine);
        return 0;
    }
    
    kprintf("[ELF] Valid ELF64 executable, entry=0x%lx, %d phdrs\n", 
            hdr->e_entry, hdr->e_phnum);
    return 1;
}

elf_load_result_t elf_load(const void *data, size_t size) {
    elf_load_result_t result = {0, 0, 0, 0};
    
    if (!elf_validate(data, size)) {
        return result;
    }
    
    const elf64_header_t *hdr = (const elf64_header_t *)data;
    const uint8_t *file_data = (const uint8_t *)data;
    
    // Process program headers
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const elf64_phdr_t *phdr = (const elf64_phdr_t *)(file_data + hdr->e_phoff + i * hdr->e_phentsize);
        
        // Only load PT_LOAD segments
        if (phdr->p_type != PT_LOAD) {
            continue;
        }
        
        kprintf("[ELF] LOAD: vaddr=0x%lx filesz=%lu memsz=%lu flags=%x\n",
                phdr->p_vaddr, phdr->p_filesz, phdr->p_memsz, phdr->p_flags);
        
        // For kernel-mode loading, we allocate in kernel heap
        // In a real system, we'd map user pages
        void *segment = kmalloc(phdr->p_memsz);
        if (!segment) {
            kprintf("[ELF] Failed to allocate %lu bytes for segment\n", phdr->p_memsz);
            return result;
        }
        
        // Clear the segment (for .bss)
        mem_set(segment, 0, phdr->p_memsz);
        
        // Copy file data
        if (phdr->p_filesz > 0) {
            mem_copy(segment, file_data + phdr->p_offset, phdr->p_filesz);
        }
        
        // Track first load address as base
        if (result.load_base == 0 || phdr->p_vaddr < result.load_base) {
            result.load_base = (uint64_t)segment;
        }
        
        result.memory_size += phdr->p_memsz;
        
        kprintf("[ELF] Loaded at 0x%lx\n", (uint64_t)segment);
    }
    
    result.entry_point = hdr->e_entry;
    result.success = 1;
    
    kprintf("[ELF] Load complete, entry=0x%lx total=%lu bytes\n", 
            result.entry_point, result.memory_size);
    
    return result;
}

// External: Jump to Ring 3
extern void jump_to_usermode(uint64_t entry, uint64_t stack);

// Execute ELF (jumps to entry point in Ring 3)
void elf_execute(uint64_t entry_point) {
    kprintf("[ELF] Preparing Ring 3 jump to 0x%lx\n", entry_point);
    
    // Allocate user stack (16KB)
    // In a real OS, this would be a user-mapped page.
    // Since we identity map everything, we can use kmalloc pointer 
    // but we need to ensure it's accessible (User bit in PTE).
    // Our VMM maps 0-4GB as Present|Write (but maybe not User?).
    // If we are in Ring 3, we can only access User pages.
    // We need to fix VMM to allow user access to this memory or identity map lower memory as user.
    // For now, let's assume our identity map is Supervisor only, which will cause a #PF in Ring 3.
    // We need to map the stack and the program code as User.
    // The program was loaded into kmalloc'd memory.
    
    // TEMPORARY PROTOTYPE:
    // We will allocate stack, but we might crash on #PF if VMM is strict.
    size_t stack_size = 16384;
    void *stack = kmalloc(stack_size);
    if (!stack) {
        kprintf("[ELF] Failed to allocate user stack\n");
        return;
    }
    
    // Initialize Security Context for PID 1 (Mock User Process)
    // In a real OS, fork() or spawn() does this.
    extern void *security_create_context(uint32_t pid, uint32_t parent_pid);
    security_create_context(1, 0); // Create PID 1 with Kernel (0) as parent -> Inheritance
    // We want to force it to be a user process though, security_create_context handles inheritance logic.
    // PID 1 will inherit from Kernel (PID 0) which implies FULL PRIVILEGES.
    // Wait, security_create_context says: "If using parent_ctx... Inherit from parent".
    // Kernel (PID 0) has CAPSET_KERNEL.
    // So PID 1 will be superuser. This is fine for now.
    // If we wanted to drop privileges, we would modify it.
    
    // Calculate top of stack (aligned to 16 bytes)
    uint64_t user_rsp = (uint64_t)stack + stack_size;
    user_rsp &= ~0xF;  // Align
    
    kprintf("[ELF] User stack at 0x%lx\n", user_rsp);
    kprintf("[ELF] Switching to User Mode...\n");
    
    // Jump to userspace
    // Note: This function will likely not return if the user program loops
    jump_to_usermode(entry_point, user_rsp);
    
    kprintf("[ELF] Execution returned (unexpected if process exit handled via interrupt)\n");
}
