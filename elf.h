#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

// ELF Magic
#define ELF_MAGIC 0x464C457F  // "\x7fELF"

// ELF Class (32 or 64 bit)
#define ELFCLASS64 2

// ELF Data encoding
#define ELFDATA2LSB 1  // Little endian

// ELF Type
#define ET_EXEC 2  // Executable
#define ET_DYN  3  // Shared object

// ELF Machine
#define EM_X86_64 62  // AMD x86-64

// Program header types
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6

// Program header flags
#define PF_X 0x1  // Execute
#define PF_W 0x2  // Write
#define PF_R 0x4  // Read

// ELF64 Header
typedef struct {
    uint8_t  e_ident[16];     // Magic, class, encoding, etc.
    uint16_t e_type;          // Object file type
    uint16_t e_machine;       // Architecture
    uint32_t e_version;       // Object file version
    uint64_t e_entry;         // Entry point virtual address
    uint64_t e_phoff;         // Program header table offset
    uint64_t e_shoff;         // Section header table offset
    uint32_t e_flags;         // Processor-specific flags
    uint16_t e_ehsize;        // ELF header size
    uint16_t e_phentsize;     // Program header entry size
    uint16_t e_phnum;         // Program header count
    uint16_t e_shentsize;     // Section header entry size
    uint16_t e_shnum;         // Section header count
    uint16_t e_shstrndx;      // Section name string table index
} __attribute__((packed)) elf64_header_t;

// ELF64 Program Header
typedef struct {
    uint32_t p_type;          // Segment type
    uint32_t p_flags;         // Segment flags
    uint64_t p_offset;        // Segment offset in file
    uint64_t p_vaddr;         // Virtual address in memory
    uint64_t p_paddr;         // Physical address (unused)
    uint64_t p_filesz;        // Size in file
    uint64_t p_memsz;         // Size in memory
    uint64_t p_align;         // Alignment
} __attribute__((packed)) elf64_phdr_t;

// ELF load result
typedef struct {
    uint64_t entry_point;     // Entry point address
    uint64_t load_base;       // Base address where loaded
    size_t   memory_size;     // Total memory used
    int      success;         // 1 if loaded, 0 on error
} elf_load_result_t;

// Validate ELF header
int elf_validate(const void *data, size_t size);

// Load ELF into memory, returns entry point (0 on failure)
elf_load_result_t elf_load(const void *data, size_t size);

// Execute loaded ELF (jumps to entry point)
void elf_execute(uint64_t entry_point);

#endif
