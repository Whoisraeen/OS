#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

// Page size
#define PAGE_SIZE 4096

// Page table entry flags
#define PTE_PRESENT    (1 << 0)
#define PTE_WRITABLE   (1 << 1)
#define PTE_USER       (1 << 2)
#define PTE_WRITETHROUGH (1 << 3)
#define PTE_NOCACHE    (1 << 4)
#define PTE_ACCESSED   (1 << 5)
#define PTE_DIRTY      (1 << 6)
#define PTE_HUGE       (1 << 7)  // 2MB page
#define PTE_GLOBAL     (1 << 8)
#define PTE_NX         (1ULL << 63)  // No-Execute (if supported)

// Address mask for page table entries (bits 12-51)
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

// Indices into page tables (each index is 9 bits)
#define PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((addr) >> 12) & 0x1FF)

void vmm_init(void);
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_user_page(uint64_t virt, uint64_t phys);
void vmm_switch(void);

// Helper to get HHDM offset (defined in vmm.c)
uint64_t vmm_get_hhdm_offset(void);

// Get PML4 physical address
uint64_t vmm_get_pml4(void);

// Create a new PML4 for a user process (maps kernel space)
uint64_t vmm_create_user_pml4(void);

#endif
