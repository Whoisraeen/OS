#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "limine/limine.h"
#include <stddef.h>

// Request kernel address from Limine
__attribute__((used, section(".requests")))
static volatile struct limine_kernel_address_request kernel_addr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

// Use PMM's memory map request (declared in pmm.c)
extern volatile struct limine_memmap_request memmap_request;

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
        uint64_t next_phys = table[index] & PTE_ADDR_MASK;
        return (uint64_t *)phys_to_virt(next_phys);
    }

    if (!allocate) {
        return NULL;
    }

    uint64_t new_table_phys = (uint64_t)pmm_alloc_page();
    if (new_table_phys == 0) {
        return NULL;
    }

    uint64_t *new_table_virt = (uint64_t *)phys_to_virt(new_table_phys);
    for (int i = 0; i < 512; i++) {
        new_table_virt[i] = 0;
    }

    table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE;
    return new_table_virt;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    size_t pml4_idx = PML4_INDEX(virt);
    size_t pdpt_idx = PDPT_INDEX(virt);
    size_t pd_idx   = PD_INDEX(virt);
    size_t pt_idx   = PT_INDEX(virt);

    uint64_t *pdpt = vmm_get_next_level(pml4_virt, pml4_idx, 1);
    if (!pdpt) return;

    uint64_t *pd = vmm_get_next_level(pdpt, pdpt_idx, 1);
    if (!pd) return;

    uint64_t *pt = vmm_get_next_level(pd, pd_idx, 1);
    if (!pt) return;

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    vmm_invlpg(virt);
}

void vmm_init(void) {
    struct limine_kernel_address_response *kernel_addr = kernel_addr_request.response;

    // Get HHDM offset from PMM (which has the Limine request)
    hhdm_offset = pmm_get_hhdm_offset();

    if (hhdm_offset == 0) {
        if (fb_ptr) for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFFAA0000;
        for (;;) __asm__("hlt");
    }

    // Allocate our PML4
    pml4_phys = (uint64_t)pmm_alloc_page();
    pml4_virt = (uint64_t *)phys_to_virt(pml4_phys);
    for (int i = 0; i < 512; i++) {
        pml4_virt[i] = 0;
    }

    // Determine highest physical address from memory map
    // Map ALL physical memory via HHDM, not just 512MB
    uint64_t highest_addr = 0;
    struct limine_memmap_response *memmap = memmap_request.response;
    if (memmap) {
        for (uint64_t i = 0; i < memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = memmap->entries[i];
            uint64_t top = entry->base + entry->length;
            if (top > highest_addr) {
                highest_addr = top;
            }
        }
    }

    // Fallback: at least 512MB
    if (highest_addr < 512 * 1024 * 1024) {
        highest_addr = 512 * 1024 * 1024;
    }

    // Cap at 4GB to avoid excessive page table allocation during early boot
    if (highest_addr > 4ULL * 1024 * 1024 * 1024) {
        highest_addr = 4ULL * 1024 * 1024 * 1024;
    }

    kprintf("[VMM] Mapping HHDM: 0 - 0x%lx (%lu MB)\n",
            highest_addr, highest_addr / (1024 * 1024));

    for (uint64_t phys = 0; phys < highest_addr; phys += PAGE_SIZE) {
        uint64_t virt = hhdm_offset + phys;
        // KERNEL SECURITY: Only PTE_WRITABLE, NO PTE_USER
        vmm_map_page(virt, phys, PTE_WRITABLE);
    }

    // Map the kernel
    if (kernel_addr != NULL) {
        uint64_t kernel_virt = kernel_addr->virtual_base;
        uint64_t kernel_phys = kernel_addr->physical_base;
        uint64_t kernel_size = 16 * 1024 * 1024; // 16 MB (generous)

        for (uint64_t offset = 0; offset < kernel_size; offset += PAGE_SIZE) {
            vmm_map_page(kernel_virt + offset, kernel_phys + offset, PTE_WRITABLE);
        }
    }

    // Map the framebuffer
    if (fb_ptr != NULL) {
        uint64_t fb_virt = (uint64_t)fb_ptr;
        uint64_t fb_phys = fb_virt - hhdm_offset;
        uint64_t fb_size = fb_width * fb_height * 4;
        fb_size = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint64_t offset = 0; offset < fb_size; offset += PAGE_SIZE) {
            vmm_map_page(fb_virt + offset, fb_phys + offset, PTE_WRITABLE);
        }
    }
}

void vmm_switch(void) {
    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r"(pml4_phys)
        : "memory"
    );
}

// Map a page with USER permissions using CURRENT CR3
void vmm_map_user_page(uint64_t virt, uint64_t phys) {
    uint64_t current_pml4_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_pml4_phys));
    current_pml4_phys &= PTE_ADDR_MASK;

    uint64_t *pml4 = (uint64_t *)phys_to_virt(current_pml4_phys);

    size_t pml4_idx = PML4_INDEX(virt);
    size_t pdpt_idx = PDPT_INDEX(virt);
    size_t pd_idx   = PD_INDEX(virt);
    size_t pt_idx   = PT_INDEX(virt);

    uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t new_phys = (uint64_t)pmm_alloc_page();
        uint64_t *new_virt = (uint64_t *)phys_to_virt(new_phys);
        for (int i = 0; i < 512; i++) new_virt[i] = 0;
        pml4[pml4_idx] = new_phys | intermediate_flags;
    } else {
        pml4[pml4_idx] |= PTE_USER;
    }

    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
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
    vmm_invlpg(virt);
}

uint64_t vmm_get_pml4(void) {
    return pml4_phys;
}

uint64_t vmm_create_user_pml4(void) {
    uint64_t phys = (uint64_t)pmm_alloc_page();
    if (!phys) return 0;

    uint64_t *virt = (uint64_t *)phys_to_virt(phys);

    // Zero lower half (User space)
    for (int i = 0; i < 256; i++) virt[i] = 0;

    // Copy upper half (Kernel space) from kernel PML4
    for (int i = 256; i < 512; i++) virt[i] = pml4_virt[i];

    return phys;
}

// ---- User-space pointer validation ----

bool is_user_address(uint64_t addr, size_t size) {
    if (size == 0) return true;
    // Check for overflow
    if (addr + size < addr) return false;
    // Must be entirely within the user canonical address range
    return (addr + size - 1) <= USER_ADDR_MAX;
}

int copy_from_user(void *kernel_dst, const void *user_src, size_t size) {
    if (!is_user_address((uint64_t)user_src, size)) return -1;
    memcpy(kernel_dst, user_src, size);
    return 0;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t size) {
    if (!is_user_address((uint64_t)user_dst, size)) return -1;
    memcpy(user_dst, kernel_src, size);
    return 0;
}

int copy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len) {
    if (!is_user_address((uint64_t)user_src, 1)) return -1;

    for (size_t i = 0; i < max_len - 1; i++) {
        // Validate each byte as we go (string might cross into kernel space)
        if (!is_user_address((uint64_t)&user_src[i], 1)) return -1;
        kernel_dst[i] = user_src[i];
        if (user_src[i] == '\0') return (int)i;
    }
    kernel_dst[max_len - 1] = '\0';
    return (int)(max_len - 1);
}
