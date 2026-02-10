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
#define ET_REL  1  // Relocatable file
#define ET_EXEC 2  // Executable
#define ET_DYN  3  // Shared object

// ELF Machine
#define EM_X86_64 62  // AMD x86-64

// Section Header Types
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_REL      9
#define SHT_SHLIB    10
#define SHT_DYNSYM   11

// Section Header Flags
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

// Symbol Bindings
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

// Symbol Types
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4

// Relocation Types (x86_64)
#define R_X86_64_NONE      0
#define R_X86_64_64        1  // S + A
#define R_X86_64_PC32      2  // S + A - P
#define R_X86_64_GOT32     3
#define R_X86_64_PLT32     4
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define R_X86_64_GOTPCREL  9
#define R_X86_64_32        10 // S + A
#define R_X86_64_32S       11 // S + A

// ELF64 Section Header
typedef struct {
    uint32_t sh_name;       // Section name (string tbl index)
    uint32_t sh_type;       // Section type
    uint64_t sh_flags;      // Section flags
    uint64_t sh_addr;       // Section virtual addr at execution
    uint64_t sh_offset;     // Section file offset
    uint64_t sh_size;       // Section size in bytes
    uint32_t sh_link;       // Link to another section
    uint32_t sh_info;       // Additional section info
    uint64_t sh_addralign;  // Section alignment
    uint64_t sh_entsize;    // Entry size if section holds table
} __attribute__((packed)) elf64_shdr_t;

// ELF64 Symbol
typedef struct {
    uint32_t st_name;       // Symbol name (string tbl index)
    uint8_t  st_info;       // Symbol type and binding
    uint8_t  st_other;      // Symbol visibility
    uint16_t st_shndx;      // Section index
    uint64_t st_value;      // Symbol value
    uint64_t st_size;       // Symbol size
} __attribute__((packed)) elf64_sym_t;

// ELF64 Relocation (with addend)
typedef struct {
    uint64_t r_offset;      // Address
    uint64_t r_info;        // Relocation type and symbol index
    int64_t  r_addend;      // Addend
} __attribute__((packed)) elf64_rela_t;

// Macros for st_info
#define ELF64_ST_BIND(i)   ((i) >> 4)
#define ELF64_ST_TYPE(i)   ((i) & 0xf)
#define ELF64_ST_INFO(b,t) (((b) << 4) + ((t) & 0xf))

// Macros for r_info
#define ELF64_R_SYM(i)     ((i) >> 32)
#define ELF64_R_TYPE(i)    ((i) & 0xffffffff)

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

// Load relocatable kernel module
int elf_load_module(const void *data, size_t size, void **entry_point);

// Load ELF into USER memory (expects CR3 to be set to user PML4), returns entry point
uint64_t elf_load_user(const void *data, size_t size);

// Execute loaded ELF (jumps to entry point)
void elf_execute(uint64_t entry_point);

#endif
