#include "elf.h"
#include "heap.h"
#include "serial.h"
#include "vmm.h"
#include "pmm.h"

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
        if (result.load_base == 0) result.load_base = (uint64_t)segment;
        
        // Track memory size
        result.memory_size += phdr->p_memsz;
    }
    
    result.entry_point = hdr->e_entry;
    result.success = 1;
    
    return result;
}

uint64_t elf_load_user(const void *data, size_t size) {
    if (!elf_validate(data, size)) return 0;
    
    const elf64_header_t *hdr = (const elf64_header_t *)data;
    const uint8_t *file_data = (const uint8_t *)data;
    
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const elf64_phdr_t *phdr = (const elf64_phdr_t *)(file_data + hdr->e_phoff + i * hdr->e_phentsize);
        
        if (phdr->p_type != PT_LOAD) continue;
        
        // Allocate pages
        uint64_t vaddr = phdr->p_vaddr;
        uint64_t memsz = phdr->p_memsz;
        uint64_t filesz = phdr->p_filesz;
        uint64_t offset = phdr->p_offset;
        
        uint64_t start_page = vaddr & ~0xFFF;
        uint64_t end_page = (vaddr + memsz + 4095) & ~0xFFF;
        
        for (uint64_t page = start_page; page < end_page; page += 4096) {
            uint64_t phys = (uint64_t)pmm_alloc_page();
            // Map with USER | RW | PRESENT (0x07)
            vmm_map_page(page, phys, 0x07);
            
            // Zero the page via HHDM
            void *kptr = (void*)(phys + vmm_get_hhdm_offset());
            mem_set(kptr, 0, 4096);
        }
        
        // Copy data to user virtual address (we are in the user CR3 context)
        // Note: Kernel can access user pages if SMAP is disabled or stac/clac is used.
        // We assume standard x86_64 behavior where Ring 0 can access Ring 3 pages.
        mem_copy((void*)vaddr, file_data + offset, filesz);
    }
    
    return hdr->e_entry;
}

void elf_execute(uint64_t entry_point) {
    // Jump to entry point
    // This assumes we are already in the correct address space (CR3)
    // and ready to execute.
    // For kernel modules, this is a direct jump.
    void (*entry)(void) = (void (*)(void))entry_point;
    entry();
}
