#include "elf.h"
#include "heap.h"
#include "serial.h"
#include "vmm.h"
#include "pmm.h"
#include "ksyms.h"
#include "string.h"

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
    
    // Check executable or shared object or relocatable
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN && hdr->e_type != ET_REL) {
        kprintf("[ELF] Not executable/relocatable (type=%d)\n", hdr->e_type);
        return 0;
    }
    
    // Check architecture
    if (hdr->e_machine != EM_X86_64) {
        kprintf("[ELF] Not x86-64 (machine=%d)\n", hdr->e_machine);
        return 0;
    }
    
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

// Helper to get section name
static const char *elf_get_section_name(const elf64_header_t *hdr, const uint8_t *data, int shndx) {
    if (hdr->e_shstrndx == 0 || hdr->e_shstrndx >= hdr->e_shnum) return NULL;
    
    const elf64_shdr_t *shstr = (const elf64_shdr_t *)(data + hdr->e_shoff + hdr->e_shstrndx * hdr->e_shentsize);
    const char *strtab = (const char *)(data + shstr->sh_offset);
    
    const elf64_shdr_t *sh = (const elf64_shdr_t *)(data + hdr->e_shoff + shndx * hdr->e_shentsize);
    return strtab + sh->sh_name;
}

// Load relocatable kernel module
int elf_load_module(const void *data, size_t size, void **entry_point) {
    if (!elf_validate(data, size)) return -1;
    
    const elf64_header_t *hdr = (const elf64_header_t *)data;
    const uint8_t *file_data = (const uint8_t *)data;
    
    if (hdr->e_type != ET_REL) {
        kprintf("[ELF] Module must be ET_REL\n");
        return -1;
    }
    
    // Allocate memory for sections
    // We need to keep track of where each section is loaded to resolve symbols
    uint64_t *section_addrs = (uint64_t *)kmalloc(hdr->e_shnum * sizeof(uint64_t));
    if (!section_addrs) return -1;
    mem_set(section_addrs, 0, hdr->e_shnum * sizeof(uint64_t));
    
    // 1. Load Allocatable Sections
    for (int i = 0; i < hdr->e_shnum; i++) {
        const elf64_shdr_t *sh = (const elf64_shdr_t *)(file_data + hdr->e_shoff + i * hdr->e_shentsize);
        
        if (sh->sh_flags & SHF_ALLOC) {
            // Allocate memory for this section
            // TODO: Alignment
            void *mem = kmalloc(sh->sh_size);
            if (!mem) {
                kprintf("[ELF] Failed to alloc section %d\n", i);
                kfree(section_addrs);
                return -1;
            }
            
            mem_set(mem, 0, sh->sh_size);
            if (sh->sh_type == SHT_PROGBITS) {
                mem_copy(mem, file_data + sh->sh_offset, sh->sh_size);
            }
            
            section_addrs[i] = (uint64_t)mem;
            // kprintf("[ELF] Section %d loaded at %p\n", i, mem);
        }
    }
    
    // 2. Perform Relocations
    for (int i = 0; i < hdr->e_shnum; i++) {
        const elf64_shdr_t *sh = (const elf64_shdr_t *)(file_data + hdr->e_shoff + i * hdr->e_shentsize);
        
        if (sh->sh_type == SHT_RELA) {
            // This section contains relocations for another section
            uint32_t target_section_idx = sh->sh_info;
            uint32_t symtab_idx = sh->sh_link;
            
            if (target_section_idx >= hdr->e_shnum || section_addrs[target_section_idx] == 0) {
                continue; // Skip if target not loaded
            }
            
            uint64_t target_base = section_addrs[target_section_idx];
            
            const elf64_shdr_t *symtab_sh = (const elf64_shdr_t *)(file_data + hdr->e_shoff + symtab_idx * hdr->e_shentsize);
            const elf64_sym_t *symtab = (const elf64_sym_t *)(file_data + symtab_sh->sh_offset);
            
            // Get string table for symbols
            const elf64_shdr_t *strtab_sh = (const elf64_shdr_t *)(file_data + hdr->e_shoff + symtab_sh->sh_link * hdr->e_shentsize);
            const char *strtab = (const char *)(file_data + strtab_sh->sh_offset);
            
            int num_relocs = sh->sh_size / sh->sh_entsize;
            const elf64_rela_t *relocs = (const elf64_rela_t *)(file_data + sh->sh_offset);
            
            for (int r = 0; r < num_relocs; r++) {
                const elf64_rela_t *rel = &relocs[r];
                
                uint32_t sym_idx = ELF64_R_SYM(rel->r_info);
                uint32_t type = ELF64_R_TYPE(rel->r_info);
                
                const elf64_sym_t *sym = &symtab[sym_idx];
                
                uint64_t sym_val = 0;
                
                if (sym->st_shndx == 0) {
                    // Undefined symbol - resolve from kernel
                    const char *name = strtab + sym->st_name;
                    void *addr = ksyms_lookup(name);
                    if (!addr) {
                        kprintf("[ELF] Undefined symbol: %s\n", name);
                        return -2;
                    }
                    sym_val = (uint64_t)addr;
                } else {
                    // Defined symbol - resolve internal address
                    if (sym->st_shndx < hdr->e_shnum) {
                        sym_val = section_addrs[sym->st_shndx] + sym->st_value;
                    }
                }
                
                uint64_t P = target_base + rel->r_offset; // Place being relocated
                uint64_t S = sym_val;                     // Symbol value
                int64_t  A = rel->r_addend;               // Addend
                
                switch (type) {
                    case R_X86_64_64:
                        *(uint64_t *)P = S + A;
                        break;
                    case R_X86_64_32:
                        *(uint32_t *)P = (uint32_t)(S + A);
                        break;
                    case R_X86_64_32S:
                        *(int32_t *)P = (int32_t)(S + A);
                        break;
                    case R_X86_64_PC32:
                        *(uint32_t *)P = (uint32_t)(S + A - P);
                        break;
                    default:
                        kprintf("[ELF] Unsupported relocation type %d\n", type);
                        break;
                }
            }
        }
    }
    
    // 3. Find Entry Point (e.g., "module_init")
    // Note: relocatable files don't use e_entry
    // We look for a symbol named "module_init" or "driver_entry"
    if (entry_point) {
        *entry_point = NULL;
        
        // Iterate over all symbol tables
        for (int i = 0; i < hdr->e_shnum; i++) {
            const elf64_shdr_t *sh = (const elf64_shdr_t *)(file_data + hdr->e_shoff + i * hdr->e_shentsize);
            if (sh->sh_type == SHT_SYMTAB) {
                const elf64_sym_t *symtab = (const elf64_sym_t *)(file_data + sh->sh_offset);
                int num_syms = sh->sh_size / sh->sh_entsize;
                
                const elf64_shdr_t *strtab_sh = (const elf64_shdr_t *)(file_data + hdr->e_shoff + sh->sh_link * hdr->e_shentsize);
                const char *strtab = (const char *)(file_data + strtab_sh->sh_offset);
                
                for (int s = 0; s < num_syms; s++) {
                    const char *name = strtab + symtab[s].st_name;
                    if (strcmp(name, "driver_entry") == 0) {
                        if (symtab[s].st_shndx < hdr->e_shnum) {
                             *entry_point = (void *)(section_addrs[symtab[s].st_shndx] + symtab[s].st_value);
                             break;
                        }
                    }
                }
            }
            if (*entry_point) break;
        }
    }
    
    kfree(section_addrs);
    return 0;
}

uint64_t elf_load_user(const void *data, size_t size) {
    if (!elf_validate(data, size)) return 0;
    
    const elf64_header_t *hdr = (const elf64_header_t *)data;
    const uint8_t *file_data = (const uint8_t *)data;
    
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const elf64_phdr_t *phdr = (const elf64_phdr_t *)(file_data + hdr->e_phoff + i * hdr->e_phentsize);
        
        if (phdr->p_type != PT_LOAD) continue;
        
        uint64_t vaddr = phdr->p_vaddr;
        uint64_t memsz = phdr->p_memsz;
        uint64_t filesz = phdr->p_filesz;
        uint64_t offset = phdr->p_offset;
        
        uint64_t start_page = vaddr & ~0xFFF;
        uint64_t end_page = (vaddr + memsz + 4095) & ~0xFFF;
        
        // Allocate and map pages, copying data as we go
        for (uint64_t page = start_page; page < end_page; page += 4096) {
            // Allocate physical page
            uint64_t phys = (uint64_t)pmm_alloc_page();
            if (!phys) {
                kprintf("[ELF] Failed to allocate page for vaddr=0x%lx\n", page);
                return 0;
            }
            
            // Zero the page via HHDM
            void *kptr = (void*)(phys + vmm_get_hhdm_offset());
            mem_set(kptr, 0, 4096);
            
            // Calculate which part of the file to copy to this page
            uint64_t page_start = page;
            uint64_t page_end = page + 4096;
            
            // Intersection of [page_start, page_end) and [vaddr, vaddr+filesz)
            uint64_t copy_start = (page_start > vaddr) ? page_start : vaddr;
            uint64_t copy_end = (page_end < vaddr + filesz) ? page_end : (vaddr + filesz);
            
            if (copy_start < copy_end) {
                uint64_t page_offset = copy_start - page_start;
                uint64_t file_offset = offset + (copy_start - vaddr);
                uint64_t copy_len = copy_end - copy_start;
                
                mem_copy((uint8_t*)kptr + page_offset, 
                         file_data + file_offset, 
                         copy_len);
            }
            
            // Map page to user virtual address (uses current CR3 = user PML4)
            vmm_map_user_page(page, phys);
        }
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
