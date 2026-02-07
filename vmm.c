#include "vmm.h"
#include "pmm.h"
#include "limine/limine.h"
#include <stddef.h>

// Request kernel address from Limine
static volatile struct limine_kernel_address_request kernel_addr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

// Our PML4 (top-level page table)
static uint64_t *pml4_virt = NULL;
static uint64_t pml4_phys = 0;
static uint64_t hhdm_offset = 0;

// Debugging globals
extern uint32_t *fb_ptr;
extern uint64_t fb_width;
extern uint64_t fb_height;

// Convert physical to virtual
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

// Convert virtual to physical (for addresses in HHDM range)
static inline uint64_t virt_to_phys(void *virt) {
    return (uint64_t)virt - hhdm_offset;
}

uint64_t vmm_get_hhdm_offset(void) {
    return hhdm_offset;
}

// Get or create a page table entry, allocating if necessary
static uint64_t *vmm_get_next_level(uint64_t *table, size_t index, int allocate) {
    if (table[index] & PTE_PRESENT) {
        // Entry exists, return the next level table
        uint64_t next_phys = table[index] & PTE_ADDR_MASK;
        return (uint64_t *)phys_to_virt(next_phys);
    }
    
    if (!allocate) {
        return NULL;
    }
    
    // Allocate a new table
    uint64_t new_table_phys = (uint64_t)pmm_alloc_page();
    if (new_table_phys == 0) {
        // Out of memory
        return NULL;
    }
    
    uint64_t *new_table_virt = (uint64_t *)phys_to_virt(new_table_phys);
    
    // Zero out the new table
    for (int i = 0; i < 512; i++) {
        new_table_virt[i] = 0;
    }
    
    // Set the entry in the parent table
    table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE;
    
    return new_table_virt;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    // Get indices
    size_t pml4_idx = PML4_INDEX(virt);
    size_t pdpt_idx = PDPT_INDEX(virt);
    size_t pd_idx   = PD_INDEX(virt);
    size_t pt_idx   = PT_INDEX(virt);
    
    // Walk the page tables, creating as needed
    uint64_t *pdpt = vmm_get_next_level(pml4_virt, pml4_idx, 1);
    if (!pdpt) return;
    
    uint64_t *pd = vmm_get_next_level(pdpt, pdpt_idx, 1);
    if (!pd) return;
    
    uint64_t *pt = vmm_get_next_level(pd, pd_idx, 1);
    if (!pt) return;
    
    // Set the final page table entry
    pt[pt_idx] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
}

void vmm_init(void) {
    struct limine_kernel_address_response *kernel_addr = kernel_addr_request.response;
    
    // Get HHDM offset from PMM (which has the Limine request)
    hhdm_offset = pmm_get_hhdm_offset();
    
    if (hhdm_offset == 0) {
        // Panic - HHDM not available
        if (fb_ptr) for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFFAA0000;
        for (;;) __asm__("hlt");
    }
    
    // Allocate our PML4
    pml4_phys = (uint64_t)pmm_alloc_page();
    pml4_virt = (uint64_t *)phys_to_virt(pml4_phys);
    
    // Zero out PML4
    for (int i = 0; i < 512; i++) {
        pml4_virt[i] = 0;
    }
    
    // Map the HHDM (Higher Half Direct Map)
    // This maps physical memory at hhdm_offset + phys
    // We'll map the first 4GB for now (using 2MB huge pages for speed)
    // But first, let's do it with 4KB pages for simplicity
    
    // Actually, for simplicity, let's just map 0-512MB of physical memory
    // This should cover most of what we need initially
    uint64_t phys_to_map = 512 * 1024 * 1024; // 512 MB
    
    for (uint64_t phys = 0; phys < phys_to_map; phys += PAGE_SIZE) {
        uint64_t virt = hhdm_offset + phys;
        // KERNEL SECURITY: Only PTE_WRITABLE, NO PTE_USER!
        // This prevents Ring 3 from accessing kernel memory
        vmm_map_page(virt, phys, PTE_WRITABLE); 
    }
    
    // Map the kernel (it's loaded at kernel_addr->virtual_base from kernel_addr->physical_base)
    // For now, let's map 16MB starting at kernel base
    if (kernel_addr != NULL) {
        uint64_t kernel_virt = kernel_addr->virtual_base;
        uint64_t kernel_phys = kernel_addr->physical_base;
        uint64_t kernel_size = 16 * 1024 * 1024; // 16 MB (generous)
        
        for (uint64_t offset = 0; offset < kernel_size; offset += PAGE_SIZE) {
            // KERNEL SECURITY: Kernel code/data must NOT be accessible to User
            vmm_map_page(kernel_virt + offset, kernel_phys + offset, PTE_WRITABLE);
        }
    }
    
    // Map the framebuffer (fb_ptr is a virtual address in HHDM)
    // The framebuffer physical address is fb_ptr - hhdm_offset
    if (fb_ptr != NULL) {
        uint64_t fb_virt = (uint64_t)fb_ptr;
        uint64_t fb_phys = fb_virt - hhdm_offset;
        uint64_t fb_size = fb_width * fb_height * 4; // 4 bytes per pixel
        
        // Round up to page size
        fb_size = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        
        for (uint64_t offset = 0; offset < fb_size; offset += PAGE_SIZE) {
            // Framebuffer needs to be USER accessible for the compositor?
            // If compositor runs in kernel, it's fine.
            // But if we want user apps to draw, we might need it.
            // Ideally, only the compositor (kernel/service) touches it.
            // Let's make it Kernel-only for now to be safe.
            vmm_map_page(fb_virt + offset, fb_phys + offset, PTE_WRITABLE);
        }
    }
}

void vmm_switch(void) {
    // Load our PML4 into CR3
    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r"(pml4_phys)
        : "memory"
    );
}

// Map a page with USER permissions (for Ring 3 access)
void vmm_map_user_page(uint64_t virt, uint64_t phys) {
    // Get indices
    size_t pml4_idx = PML4_INDEX(virt);
    size_t pdpt_idx = PDPT_INDEX(virt);
    size_t pd_idx   = PD_INDEX(virt);
    size_t pt_idx   = PT_INDEX(virt);
    
    // For user pages, we need USER bit set at ALL levels
    uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    
    // Walk the page tables, creating as needed (with USER flag)
    if (!(pml4_virt[pml4_idx] & PTE_PRESENT)) {
        uint64_t new_phys = (uint64_t)pmm_alloc_page();
        uint64_t *new_virt = (uint64_t *)phys_to_virt(new_phys);
        for (int i = 0; i < 512; i++) new_virt[i] = 0;
        pml4_virt[pml4_idx] = new_phys | intermediate_flags;
    } else {
        pml4_virt[pml4_idx] |= PTE_USER; // Ensure USER bit is set
    }
    
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4_virt[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t new_phys = (uint64_t)pmm_alloc_page();
        uint64_t *new_virt = (uint64_t *)phys_to_virt(new_phys);
        for (int i = 0; i < 512; i++) new_virt[i] = 0;
        pdpt[pdpt_idx] = new_phys | intermediate_flags;
    } else {
        pdpt[pdpt_idx] |= PTE_USER;
    }
    
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t new_phys = (uint64_t)pmm_alloc_page();
        uint64_t *new_virt = (uint64_t *)phys_to_virt(new_phys);
        for (int i = 0; i < 512; i++) new_virt[i] = 0;
        pd[pd_idx] = new_phys | intermediate_flags;
    } else {
        pd[pd_idx] |= PTE_USER;
    }
    
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);
    pt[pt_idx] = (phys & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
}

uint64_t vmm_get_pml4(void) {
    return pml4_phys;
}
